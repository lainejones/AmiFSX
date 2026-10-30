# AmiFSX

A file system exerciser for AmigaOS: a port of **fsx**, the file system
torture test from NeXT/Apple that is still used in FreeBSD, xfstests and LTP.

AmiFSX keeps an in-memory model of what a test file must contain, then
hammers the real file with random reads, writes, size changes and
close/reopens. Every read is checked byte for byte against the model, and the
file size is checked after every operation. On the first mismatch it dumps
the last 1024 operations and saves the expected content next to the test file.

It calls dos.library directly (Open, Read, Write, Seek, SetFileSize,
ExamineFH), so the handler under test sees exactly what an Amiga program
sends, with no POSIX layer in between. Needs OS 2.04 or later; runs on 68000.

## Usage

    AmiFSX FILE/A,N=NUMOPS/K/N,S=SEED/K/N,L=MAXLEN/K/N,O=OPLEN/K/N,
           R=READBDY/K/N,W=WRITEBDY/K/N,T=TRUNCBDY/K/N,C=CLOSEPROB/K/N,
           P=PROGRESS/K/N,B=BEGINOP/K/N,NOREAD/S,NOWRITE/S,NOTRUNC/S,
           NOSIZE/S,NOEXAMINE/S,XREAD/S,FILLHOLES/S,STRICT/S,CHIP/S,
           KEEP/S,Q=QUIET/S,D=DEBUG/S

| Option | fsx | Meaning |
|---|---|---|
| FILE | fname | Test file. Created empty, deleted after a clean run unless KEEP. |
| N=NUMOPS | -N | Operations to run. Default 0 = forever (Ctrl-C stops, file kept). |
| S=SEED | -S | Random seed, default 1. 0 picks one from the clock. Same seed = same run. |
| L=MAXLEN | -l | Maximum file size, default 262144. |
| O=OPLEN | -o | Maximum size of one read or write, default 65536. |
| R=, W=, T= | -r -w -t | Align read, write and size-change offsets to this many bytes. |
| C=CLOSEPROB | -c | Close and reopen the file with a 1 in N chance after each op. |
| P=PROGRESS | -p | Print a progress line every N ops. |
| B=BEGINOP | -b | With DEBUG, start printing ops at this number. |
| NOREAD, NOWRITE, NOTRUNC | -R -W | Turn off that kind of operation. |
| NOSIZE | -n | Skip the size check after each op. |
| NOEXAMINE | | Check size with Seek only, not ExamineFH. |
| XREAD | | Open a second handle and do half the reads through it (cache coherency between handles). |
| FILLHOLES | | Extend the file by writing zeroes instead of SetFileSize (for handlers without SetFileSize). |
| STRICT | | Space added by SetFileSize must read back as zeroes. |
| CHIP | | Put the data buffers in Chip RAM (DMA path testing). |
| D=DEBUG | -d | Print every operation. |

Return codes: 0 passed, 5 stopped with Ctrl-C, 10 test failure, 20 setup error.

Example soak test on a hard disk partition:

    AmiFSX DH1:fsxtest N=100000 P=5000 C=100 XREAD

## Differences from POSIX fsx

- **No seeking past EOF.** AmigaDOS Seek can't go beyond end of file, so a
  write that starts past EOF first extends the file with SetFileSize.
- **Extended space is undefined by default.** AmigaDOS doesn't promise that
  SetFileSize zero-fills, and FFS doesn't. It hands back old disk blocks. So
  the first read of such bytes adopts whatever comes back, and every later read
  must return the same bytes. STRICT demands zeroes like POSIX ftruncate.
- **Other handles are parked before a size change.** With XREAD, the second
  handle is moved to offset 0 before SetFileSize. Some handlers refuse to cut
  a file below another handle's position and return the size they did set.
  WinUAE's host file system works this way.
- **No mmap operations.** AmigaOS has no mmap.
- **Data pattern.** Even offsets hold the operation number mod 256, so the
  "Step#" line in a failure report names the operation that wrote a bad byte.
  Write buffers come from arbitrary offsets in the model, so the device sees
  odd and unaligned buffer addresses too.

## When it fails

You get the bad byte ranges, a guess at which step wrote the bad data, and
the operation log. Two files are written next to the test file:

- `<file>.fsxlog`: the operation log.
- `<file>.fsxgood`: what the file should contain. Compare it with the test file.

Re-run with the same SEED to reproduce. Add DEBUG and BEGINOP to watch the
operations leading up to the failure.

## Test results so far

Tested in WinUAE, OS 3.2, 68030:

| Target | Plain | XREAD |
|---|---|---|
| FFS on RAD: (ramdrive.device) | pass, 5000 ops | pass, 5000 ops |
| WinUAE host directory (DH0:) | pass | pass, 5000 ops |
| RAM: (ram-handler) | pass, 5000 ops | **fail**: second handle can't Seek into space the first handle added |
| vamos (host) | pass, 3000 ops, needs NOTRUNC FILLHOLES NOEXAMINE | **fail**: second handle reads stale data |

FFS with STRICT fails, because extended space holds old block contents.
That's legal AmigaDOS behavior, not a bug.

`tools/RAD-FFS` is a mountfile for an 880K FFS ramdrive, handy for a quick FFS run:

    Mount RAD: FROM tools/RAD-FFS
    Format DRIVE RAD: NAME FSXFFS FFS QUICK NOICONS
    AmiFSX RAD:fsxtest N=5000 XREAD

## Building

amiga-gcc (Bebbo), from WSL:

    cd /mnt/c/projects/AmiFSX && ./build.sh

## Credits

fsx was written by Avadis Tevanian, Jr. at NeXT/Apple and extended by many
people since. AmiFSX reimplements its algorithm, option set and log format
for AmigaDOS.

## License

MIT, see LICENSE. AmiFSX is a new implementation of the fsx algorithm and
contains no fsx source code.
