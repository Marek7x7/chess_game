Headless driver for the SDL GUI (no display needed): `shim.c` hooks `SDL_RenderPresent` and, each frame,
runs the commands in the file named by `$SHIM_CMD` (`click X Y`, `key NAME`, `clip TEXT`, `shot PATH`, `quit`).
`drive.py` is an example session (two-player game, threefold repetition, undo, flip, PGN save, FEN copy/paste).

    gcc -shared -fPIC -o shim.so shim.c $(pkg-config --cflags sdl2) -ldl
    SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software LD_PRELOAD=$PWD/shim.so SHIM_CMD=/tmp/cmd ../../build-release/chess

The screenshots are BMP files; read them to check the result (there are no assertions).
