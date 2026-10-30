/*
 * AmiFSX - file system exerciser for AmigaOS (port of fsx)
 *
 * fsx was written by Avadis Tevanian, Jr. at NeXT/Apple and later extended
 * in FreeBSD, xfstests and LTP.  This is a port of its algorithm onto native
 * dos.library calls (Open/Read/Write/Seek/SetFileSize/ExamineFH), so the
 * file system handler under test sees exactly what an Amiga program does,
 * with no POSIX emulation layer in between.
 *
 * The idea: keep an in-memory copy ("good buffer") of what the test file
 * must contain, then hammer the real file with random reads, writes and
 * size changes.  Every read is compared byte for byte against the model and
 * the file size is checked after every operation.  On the first mismatch
 * the last LOGSIZE operations are dumped, and the expected content is saved
 * next to the test file as <file>.fsxgood for comparison.
 *
 * AmigaDOS differences from POSIX fsx:
 *  - Seek() cannot move past end of file, so a write that starts beyond EOF
 *    first extends the file with SetFileSize() and then writes.
 *  - SetFileSize() does not promise that extended space is zero-filled
 *    (FFS hands back whatever the newly allocated blocks contained).  By
 *    default such bytes are "undefined": the first read adopts whatever the
 *    file system returns, and every later read must return the same bytes.
 *    STRICT requires zeroes, like POSIX ftruncate().
 *
 * Requires dos.library 37 (OS 2.04) or later.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

static const char version[] = "$VER: AmiFSX 1.0 (30.10.2026)";

#define TEMPLATE \
    "FILE/A,N=NUMOPS/K/N,S=SEED/K/N,L=MAXLEN/K/N,O=OPLEN/K/N," \
    "R=READBDY/K/N,W=WRITEBDY/K/N,T=TRUNCBDY/K/N,C=CLOSEPROB/K/N," \
    "P=PROGRESS/K/N,B=BEGINOP/K/N,NOREAD/S,NOWRITE/S,NOTRUNC/S," \
    "NOSIZE/S,NOEXAMINE/S,XREAD/S,FILLHOLES/S,STRICT/S,CHIP/S,KEEP/S,Q=QUIET/S,D=DEBUG/S"

enum {
    A_FILE, A_NUMOPS, A_SEED, A_MAXLEN, A_OPLEN, A_READBDY, A_WRITEBDY,
    A_TRUNCBDY, A_CLOSEPROB, A_PROGRESS, A_BEGINOP, A_NOREAD, A_NOWRITE,
    A_NOTRUNC, A_NOSIZE, A_NOEXAMINE, A_XREAD, A_FILLHOLES, A_STRICT, A_CHIP, A_KEEP,
    A_QUIET, A_DEBUG, A_COUNT
};

/* ------------------------------------------------------------------ */
/* configuration and state                                             */

static ULONG maxfilelen = 256 * 1024;
static ULONG maxoplen   = 64 * 1024;
static ULONG readbdy = 1, writebdy = 1, truncbdy = 1;
static ULONG closeprob, progress, beginop, numops;
static BOOL  doreads = TRUE, dowrites = TRUE, dotrunc = TRUE;
static BOOL  dosize = TRUE, doexamine = TRUE, xread, fillholes, strict, quiet, debug;

static char   fname[256];
static BPTR   fh, fh2;              /* main handle, second handle (XREAD) */
static struct FileInfoBlock *fib;

static UBYTE *good;                 /* expected file content */
static UBYTE *temp;                 /* read buffer */
static UBYTE *undef;                /* bitmap: byte content not yet known */
static ULONG  file_size;
static ULONG  testcalls;
static ULONG  rngstate = 1;
static ULONG  stale_reads;          /* reads that adopted non-zero undefined bytes */

#define UNDEF(o) (undef[(o) >> 3] & (1 << ((o) & 7)))

/* ------------------------------------------------------------------ */
/* operation log                                                       */

#define LOGSIZE 1024

enum { L_READ = 1, L_XREAD, L_WRITE, L_TRUNC, L_CLOSEOPEN, L_SKIP };
#define LF_HOLE   1     /* write started past EOF */
#define LF_EXTEND 2     /* write ended past EOF */

struct logent {
    ULONG step;
    ULONG a, b;
    UBYTE op, flags;
};

static struct logent oplog[LOGSIZE];
static ULONG logcount;

static void format_entry(const struct logent *le, char *buf)
{
    int n = sprintf(buf, "%u(%3u mod 256): ", le->step, le->step & 0xff);
    buf += n;

    switch (le->op) {
    case L_READ:
    case L_XREAD:
        sprintf(buf, "%s 0x%05x thru 0x%05x\t(0x%x bytes)",
                le->op == L_READ ? "READ    " : "XREAD   ",
                le->a, le->a + le->b - 1, le->b);
        break;
    case L_WRITE:
        sprintf(buf, "WRITE    0x%05x thru 0x%05x\t(0x%x bytes)%s%s",
                le->a, le->a + le->b - 1, le->b,
                (le->flags & LF_HOLE) ? " HOLE" : "",
                (le->flags & LF_EXTEND) ? " EXTEND" : "");
        break;
    case L_TRUNC:
        sprintf(buf, "TRUNCATE %s\tfrom 0x%x to 0x%x",
                le->b > le->a ? "UP  " : "DOWN", le->a, le->b);
        break;
    case L_CLOSEOPEN:
        strcpy(buf, "CLOSE/OPEN");
        break;
    default:
        strcpy(buf, "SKIPPED (no operation)");
        break;
    }
}

static void logop(UBYTE op, ULONG a, ULONG b, UBYTE flags)
{
    struct logent *le = &oplog[logcount % LOGSIZE];

    le->step = testcalls;
    le->op = op;
    le->a = a;
    le->b = b;
    le->flags = flags;
    logcount++;

    if (debug && testcalls >= beginop) {
        char line[128];
        format_entry(le, line);
        printf("%s\n", line);
    }
}

/* print the log to stdout and, if lf is open, to the .fsxlog file */
static void dump_log(BPTR lf)
{
    char line[128];
    ULONG i, count, start;

    count = logcount < LOGSIZE ? logcount : LOGSIZE;
    start = logcount - count;

    printf("LOG DUMP (%u total operations):\n", logcount);
    for (i = start; i < logcount; i++) {
        format_entry(&oplog[i % LOGSIZE], line);
        printf("%s\n", line);
        if (lf) {
            FPuts(lf, line);
            FPutC(lf, '\n');
        }
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static ULONG rnd(void)
{
    /* xorshift32: same sequence on every Amiga for a given seed */
    ULONG x = rngstate;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rngstate = x;
}

static void ioerr(const char *what)
{
    char msg[100];
    LONG err = IoErr();

    if (!Fault(err, NULL, msg, sizeof(msg)))
        msg[0] = 0;
    printf("%s failed: error %d (%s)\n", what, err, msg);
}

static void undef_set(ULONG off, ULONG len, BOOL val)
{
    ULONG end = off + len;

    while (off < end && (off & 7)) {
        if (val) undef[off >> 3] |=  (1 << (off & 7));
        else     undef[off >> 3] &= ~(1 << (off & 7));
        off++;
    }
    if (end - off >= 8) {
        ULONG n = (end - off) >> 3;
        memset(undef + (off >> 3), val ? 0xff : 0, n);
        off += n << 3;
    }
    while (off < end) {
        if (val) undef[off >> 3] |=  (1 << (off & 7));
        else     undef[off >> 3] &= ~(1 << (off & 7));
        off++;
    }
}

/*
 * Mark [from, to) as newly added space: zero in the model, and undefined
 * unless STRICT.
 */
static void grow_model(ULONG from, ULONG to)
{
    memset(good + from, 0, to - from);
    undef_set(from, to - from, !strict);
}

/*
 * Fill the model with this step's data.  Even offsets hold the step number
 * mod 256, so a bad byte tells you which operation wrote it; odd offsets
 * mix in the offset, so shifted data does not look correct.
 */
static void gendata(ULONG off, ULONG size)
{
    UBYTE step = (UBYTE)testcalls;
    ULONG o, end = off + size;

    for (o = off; o < end; o++)
        good[o] = (o & 1) ? (UBYTE)(step ^ (o >> 1) ^ 0xa5) : step;
    undef_set(off, size, FALSE);
}

static void report_failure(UBYTE *buf, ULONG off, ULONG size, const char *what)
{
    ULONG i = 0, runs = 0;
    BOOL guessed = FALSE;

    printf("%s BAD DATA: offset = 0x%x, size = 0x%x, fname = %s\n",
           what, off, size, fname);
    printf("OFFSET\tGOOD\tBAD\tRANGE\n");

    while (i < size) {
        ULONG start;

        if (buf[i] == good[off + i] || UNDEF(off + i)) {
            i++;
            continue;
        }
        start = i;
        while (i < size && buf[i] != good[off + i] && !UNDEF(off + i))
            i++;
        if (runs < 16)
            printf("0x%05x\t0x%02x\t0x%02x\t0x%x\n", off + start,
                   good[off + start], buf[start], i - start);
        if (!guessed) {
            ULONG e = ((off + start) & 1) ? start + 1 : start;
            if (e < i) {
                printf("Step# (mod 256) for a bad data may be %u\n", buf[e]);
                guessed = TRUE;
            }
        }
        runs++;
    }
    if (runs > 16)
        printf("... %u bad ranges in total\n", runs);
}

/*
 * Compare a read buffer with the model.  Undefined bytes (space added by
 * SetFileSize) adopt what was read, so later reads must return the same.
 */
static BOOL check_buffers(UBYTE *buf, ULONG off, ULONG size, const char *what)
{
    ULONG i;
    BOOL stale = FALSE;

    if (memcmp(buf, good + off, size) != 0) {
        for (i = 0; i < size; i++) {
            if (buf[i] == good[off + i])
                continue;
            if (!UNDEF(off + i)) {
                report_failure(buf, off, size, what);
                return FALSE;
            }
        }
        for (i = 0; i < size; i++) {
            if (buf[i] != good[off + i]) {
                good[off + i] = buf[i];
                stale = TRUE;
            }
        }
    }
    if (stale)
        stale_reads++;
    undef_set(off, size, FALSE);
    return TRUE;
}

static BOOL check_size(void)
{
    LONG sz;

    if (Seek(fh, 0, OFFSET_END) < 0) {
        ioerr("Seek to end");
        return FALSE;
    }
    sz = Seek(fh, 0, OFFSET_BEGINNING);
    if (sz < 0) {
        ioerr("Seek to start");
        return FALSE;
    }
    if ((ULONG)sz != file_size) {
        printf("Size error: expected 0x%x but Seek found 0x%x\n",
               file_size, (ULONG)sz);
        return FALSE;
    }
    if (doexamine) {
        if (!ExamineFH(fh, fib)) {
            ioerr("ExamineFH");
            return FALSE;
        }
        if ((ULONG)fib->fib_Size != file_size) {
            printf("Size error: expected 0x%x but ExamineFH found 0x%x\n",
                   file_size, (ULONG)fib->fib_Size);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL open_handles(void)
{
    fh = Open(fname, MODE_OLDFILE);
    if (!fh) {
        ioerr("Open");
        return FALSE;
    }
    if (xread) {
        fh2 = Open(fname, MODE_OLDFILE);
        if (!fh2) {
            ioerr("Open of second handle");
            return FALSE;
        }
    }
    return TRUE;
}

static void close_handles(void)
{
    if (fh2) { Close(fh2); fh2 = 0; }
    if (fh)  { Close(fh);  fh = 0; }
}

/* ------------------------------------------------------------------ */
/* operations                                                          */

static BOOL doread(void)
{
    ULONG off, size;
    BOOL viax = xread && (rnd() & 1);
    BPTR h = viax ? fh2 : fh;
    LONG got;

    if (file_size == 0) {
        logop(L_SKIP, 0, 0, 0);
        return TRUE;
    }
    off = rnd() % file_size;
    off -= off % readbdy;
    size = rnd() % (maxoplen + 1);
    if (off + size > file_size)
        size = file_size - off;
    if (size == 0) {
        logop(L_SKIP, 0, 0, 0);
        return TRUE;
    }
    logop(viax ? L_XREAD : L_READ, off, size, 0);

    if (Seek(h, off, OFFSET_BEGINNING) < 0) {
        ioerr("Seek before read");
        return FALSE;
    }
    got = Read(h, temp, size);
    if (got < 0) {
        ioerr("Read");
        return FALSE;
    }
    if ((ULONG)got != size) {
        printf("Short read: 0x%x bytes instead of 0x%x\n", (ULONG)got, size);
        return FALSE;
    }
    return check_buffers(temp, off, size, viax ? "XREAD" : "READ");
}

/*
 * Handlers may refuse to truncate below another open handle's position
 * (WinUAE's host file system does, and returns the size it really set).
 * Move the XREAD handle to the start so SetFileSize is unambiguous.
 */
static void park_second(void)
{
    if (fh2)
        Seek(fh2, 0, OFFSET_BEGINNING);
}

/* write zeroes over [from, to) of the real file, using temp as the source */
static BOOL fill_zero(ULONG from, ULONG to)
{
    ULONG chunk = maxoplen ? maxoplen : 1;
    LONG r;

    memset(temp, 0, chunk);
    if (Seek(fh, from, OFFSET_BEGINNING) < 0) {
        ioerr("Seek before hole fill");
        return FALSE;
    }
    while (from < to) {
        ULONG n = to - from;
        if (n > chunk)
            n = chunk;
        r = Write(fh, temp, n);
        if (r != (LONG)n) {
            if (r < 0)
                ioerr("Write of hole fill");
            else
                printf("Short write in hole fill: 0x%x instead of 0x%x\n",
                       (ULONG)r, n);
            return FALSE;
        }
        from += n;
    }
    return TRUE;
}

static BOOL dowrite(void)
{
    ULONG off, size;
    UBYTE flags = 0;
    LONG r;

    off = rnd() % maxfilelen;
    off -= off % writebdy;
    size = rnd() % (maxoplen + 1);
    if (off + size > maxfilelen)
        size = maxfilelen - off;
    if (size == 0) {
        logop(L_SKIP, 0, 0, 0);
        return TRUE;
    }
    if (off > file_size)
        flags |= LF_HOLE;
    if (off + size > file_size)
        flags |= LF_EXTEND;
    logop(L_WRITE, off, size, flags);

    if (off > file_size) {
        if (fillholes) {
            /* extend by writing zeroes instead of using SetFileSize */
            if (!fill_zero(file_size, off))
                return FALSE;
            memset(good + file_size, 0, off - file_size);
            undef_set(file_size, off - file_size, FALSE);
            file_size = off;
            goto data;
        }
        /* AmigaDOS cannot Seek past EOF: extend the file first */
        park_second();
        r = SetFileSize(fh, off, OFFSET_BEGINNING);
        if (r < 0) {
            ioerr("SetFileSize (extend before write)");
            return FALSE;
        }
        if ((ULONG)r != off) {
            printf("SetFileSize returned 0x%x, expected 0x%x\n", (ULONG)r, off);
            return FALSE;
        }
        grow_model(file_size, off);
        file_size = off;
    }

data:
    gendata(off, size);
    if (Seek(fh, off, OFFSET_BEGINNING) < 0) {
        ioerr("Seek before write");
        return FALSE;
    }
    r = Write(fh, good + off, size);
    if (r < 0) {
        ioerr("Write");
        return FALSE;
    }
    if ((ULONG)r != size) {
        printf("Short write: 0x%x bytes instead of 0x%x\n", (ULONG)r, size);
        return FALSE;
    }
    if (off + size > file_size)
        file_size = off + size;
    return TRUE;
}

static BOOL dotruncate(void)
{
    ULONG newsize;
    LONG r;

    newsize = rnd() % (maxfilelen + 1);
    newsize -= newsize % truncbdy;
    logop(L_TRUNC, file_size, newsize, 0);

    park_second();
    r = SetFileSize(fh, newsize, OFFSET_BEGINNING);
    if (r < 0) {
        ioerr("SetFileSize");
        return FALSE;
    }
    if ((ULONG)r != newsize) {
        printf("SetFileSize returned 0x%x, expected 0x%x\n", (ULONG)r, newsize);
        return FALSE;
    }
    if (newsize > file_size)
        grow_model(file_size, newsize);
    file_size = newsize;
    return TRUE;
}

static BOOL docloseopen(void)
{
    logop(L_CLOSEOPEN, 0, 0, 0);
    close_handles();
    return open_handles();
}

/* read back the whole file at the end of a run */
static BOOL check_all(void)
{
    ULONG off = 0;

    while (off < file_size) {
        ULONG size = file_size - off;
        LONG got;

        if (size > maxoplen)
            size = maxoplen;
        if (Seek(fh, off, OFFSET_BEGINNING) < 0) {
            ioerr("Seek in final check");
            return FALSE;
        }
        got = Read(fh, temp, size);
        if (got < 0) {
            ioerr("Read in final check");
            return FALSE;
        }
        if ((ULONG)got != size) {
            printf("Short read in final check: 0x%x instead of 0x%x\n",
                   (ULONG)got, size);
            return FALSE;
        }
        if (!check_buffers(temp, off, size, "FINAL READ"))
            return FALSE;
        off += size;
    }
    return TRUE;
}

/* write <file>.fsxgood and <file>.fsxlog after a failure */
static void save_evidence(void)
{
    char path[sizeof(fname) + 10];
    BPTR f;

    sprintf(path, "%s.fsxlog", fname);
    f = Open(path, MODE_NEWFILE);
    dump_log(f);
    if (f)
        Close(f);
    else
        ioerr("Open of .fsxlog");

    sprintf(path, "%s.fsxgood", fname);
    f = Open(path, MODE_NEWFILE);
    if (!f) {
        ioerr("Open of .fsxgood");
        return;
    }
    if (file_size && Write(f, good, file_size) != (LONG)file_size)
        ioerr("Write of .fsxgood");
    Close(f);
    printf("Correct content saved for comparison\n"
           "(compare \"%s\" with \"%s\")\n", fname, path);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    LONG args[A_COUNT];
    struct RDArgs *rda;
    int rc = RETURN_FAIL;
    ULONG memflags;
    BOOL ok = TRUE, broke = FALSE;

    (void)version;

    if (((struct Library *)DOSBase)->lib_Version < 37) {
        printf("AmiFSX needs AmigaOS 2.04 or later\n");
        return RETURN_FAIL;
    }

    memset(args, 0, sizeof(args));
    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), "AmiFSX");
        return RETURN_FAIL;
    }

    strncpy(fname, (char *)args[A_FILE], sizeof(fname) - 1);
    if (args[A_NUMOPS])    numops     = *(LONG *)args[A_NUMOPS];
    if (args[A_SEED])      rngstate   = *(LONG *)args[A_SEED];
    if (args[A_MAXLEN])    maxfilelen = *(LONG *)args[A_MAXLEN];
    if (args[A_OPLEN])     maxoplen   = *(LONG *)args[A_OPLEN];
    if (args[A_READBDY])   readbdy    = *(LONG *)args[A_READBDY];
    if (args[A_WRITEBDY])  writebdy   = *(LONG *)args[A_WRITEBDY];
    if (args[A_TRUNCBDY])  truncbdy   = *(LONG *)args[A_TRUNCBDY];
    if (args[A_CLOSEPROB]) closeprob  = *(LONG *)args[A_CLOSEPROB];
    if (args[A_PROGRESS])  progress   = *(LONG *)args[A_PROGRESS];
    if (args[A_BEGINOP])   beginop    = *(LONG *)args[A_BEGINOP];
    doreads   = !args[A_NOREAD];
    dowrites  = !args[A_NOWRITE];
    dotrunc   = !args[A_NOTRUNC];
    dosize    = !args[A_NOSIZE];
    doexamine = !args[A_NOEXAMINE];
    xread     = args[A_XREAD] != 0;
    fillholes = args[A_FILLHOLES] != 0;
    strict    = args[A_STRICT] != 0;
    quiet     = args[A_QUIET] != 0;
    debug     = args[A_DEBUG] != 0;
    memflags  = (args[A_CHIP] ? MEMF_CHIP : MEMF_ANY) | MEMF_CLEAR;

    if (rngstate == 0) {
        struct DateStamp ds;
        DateStamp(&ds);
        rngstate = (ULONG)ds.ds_Days * 4320000UL
                 + (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
        if (rngstate == 0)
            rngstate = 1;
    }

    if ((LONG)maxfilelen <= 0 || (LONG)maxoplen < 0 ||
        (LONG)readbdy <= 0 || (LONG)writebdy <= 0 || (LONG)truncbdy <= 0) {
        printf("MAXLEN and the boundaries must be at least 1\n");
        goto out;
    }
    if (maxoplen > maxfilelen)
        maxoplen = maxfilelen;
    if (!doreads && !dowrites && !dotrunc) {
        printf("NOREAD, NOWRITE and NOTRUNC together leave nothing to do\n");
        goto out;
    }

    good  = AllocVec(maxfilelen, memflags);
    temp  = AllocVec(maxoplen ? maxoplen : 1, memflags);
    undef = AllocVec((maxfilelen >> 3) + 1, MEMF_ANY | MEMF_CLEAR);
    fib   = AllocDosObject(DOS_FIB, NULL);
    if (!good || !temp || !undef || !fib) {
        printf("Out of memory (try a smaller MAXLEN or OPLEN)\n");
        goto out;
    }

    /* create an empty file, then reopen shared so XREAD can add a handle */
    fh = Open(fname, MODE_NEWFILE);
    if (!fh) {
        ioerr("Creating test file");
        goto out;
    }
    Close(fh);
    fh = 0;
    if (!open_handles())
        goto out;

    printf("AmiFSX: %s, maxlen 0x%x, oplen 0x%x, seed %u, %s\n",
           fname, maxfilelen, maxoplen, rngstate,
           strict ? "strict" : "extended space undefined");
    fflush(stdout);

    rc = RETURN_ERROR;
    while (numops == 0 || testcalls < numops) {
        ULONG pick;

        if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            broke = TRUE;
            break;
        }
        testcalls++;

        /* reads and writes 2/5 each, size changes 1/5 */
        pick = rnd() % 5;
        if (pick < 2)
            ok = doreads ? doread() : (logop(L_SKIP, 0, 0, 0), TRUE);
        else if (pick < 4)
            ok = dowrites ? dowrite() : (logop(L_SKIP, 0, 0, 0), TRUE);
        else
            ok = dotrunc ? dotruncate() : (logop(L_SKIP, 0, 0, 0), TRUE);

        if (ok && closeprob && rnd() % closeprob == 0)
            ok = docloseopen();
        if (ok && dosize)
            ok = check_size();
        if (!ok)
            break;

        if (progress && testcalls % progress == 0 && !quiet) {
            printf("%u ops, file size 0x%x\n", testcalls, file_size);
            fflush(stdout);
        }
    }

    if (broke) {
        printf("*** Break after %u operations; test file kept\n", testcalls);
        rc = RETURN_WARN;
        goto out;
    }
    if (ok)
        ok = check_all();
    if (!ok) {
        printf("Failure at operation %u\n", testcalls);
        save_evidence();
        goto out;
    }

    printf("All %u operations completed A-OK!\n", testcalls);
    if (stale_reads)
        printf("Note: %u reads found non-zero data in space added by "
               "SetFileSize (stale disk blocks).\n"
               "That is legal in AmigaDOS; add STRICT to treat it as a failure.\n",
               stale_reads);
    rc = RETURN_OK;

out:
    close_handles();
    if (rc == RETURN_OK && !args[A_KEEP])
        DeleteFile(fname);
    if (fib)   FreeDosObject(DOS_FIB, fib);
    if (undef) FreeVec(undef);
    if (temp)  FreeVec(temp);
    if (good)  FreeVec(good);
    FreeArgs(rda);
    return rc;
}
