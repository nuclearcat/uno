# UNO

UNO is a small terminal text editor written in C. It uses Nano-inspired
keybindings and has no ncurses dependency. The complete editor implementation
is contained in [`uno.c`](uno.c).

```text
┌─ uno ──────────────────────────────────────────────────────────────┐
│ #include <stdio.h>                                                 │
│                                                                   │
│ int main(void) {                                                  │
│     puts("small tools are beautiful");                            │
│     return 0;                                                     │
│ }                                                                 │
├───────────────────────────────────────────────────────────────────┤
│ hello.c - 7 lines (modified)                                4:17  │
│ ^O Save  ^X Exit  ^W Find  ^\ Replace  ^K Cut  ^U Paste  F3 Next │
└───────────────────────────────────────────────────────────────────┘
```

## Features

- Insert and delete text, split and join lines, and navigate with arrow, Home,
  End, Page Up, and Page Down keys.
- Search from the cursor with document wrapping and repeat searches with F3.
- Replace matches individually or all at once. An empty replacement deletes
  matching text.
- Cut and paste lines with Nano-style shortcuts. Consecutive cuts accumulate so
  they can be pasted together.
- Save existing or newly named files.
- Confirm whether to save, discard, or cancel when quitting with unsaved work.
- Scroll vertically and horizontally, expand tabs, and adapt to terminal size.
- Build from one C source file using standard C and POSIX terminal APIs.

## Build

UNO requires a C11 compiler and a POSIX-like system such as Linux, macOS, or a
BSD.

```sh
git clone https://github.com/nuclearcat/uno.git
cd uno
make
```

To supply your own compiler or flags:

```sh
make CC=clang CFLAGS='-std=c11 -Wall -Wextra -Os'
```

Remove the compiled executable with:

```sh
make clean
```

## Usage

Open an existing file:

```sh
./uno notes.txt
```

Or start with an unnamed, empty buffer:

```sh
./uno
```

Press `Ctrl-O` to choose a filename when saving an unnamed buffer.

## Keybindings

| Action | Key |
|---|---|
| Save | `Ctrl-O` |
| Quit | `Ctrl-X` |
| Search | `Ctrl-W` |
| Find next | `F3` |
| Search and replace | `Ctrl-\` |
| Cut current line (consecutive cuts accumulate) | `Ctrl-K` |
| Paste cut line(s) | `Ctrl-U` |
| Backspace | `Backspace` or `Ctrl-H` |
| Delete forward | `Delete` |
| Start/end of line | `Home` / `End` |
| Move one screen | `Page Up` / `Page Down` |
| Redraw screen | `Ctrl-L` |
| Cancel a prompt | `Esc` |

During replacement, choose:

| Key | Meaning |
|---|---|
| `y` | Replace this match |
| `n` | Skip this match |
| `a` | Replace this and all remaining matches |
| `q` or `Esc` | Stop replacing |

## Project layout

```text
uno/
├── uno.c       # Complete editor implementation
├── Makefile    # Build and clean targets
├── README.md   # You are here
└── LICENSE     # Public-domain dedication and disclaimer
```

## License

UNO is released into the public domain under the [Unlicense](LICENSE). It is
provided **as is**, without warranty, and without liability for damages arising
from its use.
