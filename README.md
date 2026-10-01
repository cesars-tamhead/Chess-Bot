# Sierra Chess — three-screen app

## Quick start — no compilation needed

Open `index.html` in a modern browser and click **Play**. Legal moves and the bot run directly in the browser. The app still works without the local C++ server. Where supported, a Web Worker keeps bot thinking off the UI thread.

## Optional: run the C++ engine on macOS or Linux

Open a terminal in this folder:

```bash
g++ -std=c++17 -O3 chessapp.cpp -o chessapp
./chessapp
```

If macOS has no compiler, `xcode-select --install` installs Apple's compiler; you can use `clang++` instead of `g++`.

Open **http://127.0.0.1:8765** in your browser. Keep the terminal running. Ctrl+C stops the app.

## Run on Windows with MinGW-w64

```bash
g++ -std=c++17 -O3 chessapp.cpp -o chessapp.exe -lws2_32
chessapp.exe
```

Windows source is included but was not executed in Windows during verification. No Python, JavaScript packages, or online service is required. The standalone HTML also needs no compiler.

## Screens

1. Main menu: Play, Settings, and How to play.
2. Game: clickable legal moves, color and difficulty selectors, promotion, move history, Undo turn, Flip, Resign, and eligible draw claims.
3. Result: a win or loss by checkmate, a loss by resignation, or a draw. Play again resets screen 2 with the same settings; Main menu returns to screen 1.

Changing color or difficulty during a game restarts the board after confirmation. Resign asks for confirmation and shows a loss result. Game controls are disabled during a bot search. Board buttons also work with keyboard Tab and Enter/Space.

## Engine behavior

Easy searches up to depth 2. Medium searches up to depth 4. Hard searches as deeply as time allows. All modes have a 2.8-second soft target and a 3.2-second hard search deadline. Easier modes and detected mates can finish sooner. Scheduling can cause slight timing overruns. No Elo rating or perfect-play guarantee is claimed.

When opened directly, HTML uses an embedded JavaScript engine for legal moves, checkmate, draw outcomes, and bot search. When served by chessapp, the same interface uses the original C++ engine instead. JavaScript manages the three visual screens and resignation. Move history uses coordinates rather than full standard algebraic notation.

## Files

- index.html: complete HTML/CSS/JavaScript app with section comments. Opening it directly runs the complete playable app; it can also use the C++ engine when served by chessapp.
- chessapp.cpp: original commented engine plus the local HTTP application server. All C++ needed to run the app is here.
- README.md: these instructions.

Keep index.html beside the compiled executable. If port 8765 is busy, use `./chessapp --serve 8766` and open the new address.

## Tests and limits

`./chessapp --test` runs the engine regression checks. `./chessapp --search 3200` runs a terminal search. `./chessapp --perft 4` checks legal move-sequence counts. `./chessapp --api state "FEN" ""` emits board JSON.

Game state lives only in the current tab. The optional C++ server binds only to 127.0.0.1 and processes one request at a time. Refresh resets the game. Draw recognition covers common insufficient-material cases rather than every theoretical dead position. HTML offers current-position draw claims; terminal engine logic also supports claims by intended move.
