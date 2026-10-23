// LD_PRELOAD shim: each SDL_RenderPresent reads $SHIM_CMD, runs its commands and deletes it.
// click X Y | key C | clip TEXT | shot PATH | quit
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static void (*real)(SDL_Renderer*);
void SDL_RenderPresent(SDL_Renderer* r) {
    if (!real) { real = dlsym(RTLD_NEXT, "SDL_RenderPresent"); setvbuf(stdout, 0, _IOLBF, 0); }
    real(r);
    const char* path = getenv("SHIM_CMD");
    FILE* f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[2048];
    static char buf[8][2048]; int n = 0;
    while (fgets(line, sizeof line, f) && n < 8) { strcpy(buf[n++], line); }
    fclose(f); unlink(path);
    for (int i = 0; i < n; i++) {
        char* l = buf[i]; l[strcspn(l, "\n")] = 0;
        int x, y; char arg[1900];
        if (sscanf(l, "click %d %d", &x, &y) == 2) {
            SDL_Event e; memset(&e, 0, sizeof e);
            e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; SDL_PushEvent(&e);
            e.type = SDL_MOUSEBUTTONUP; SDL_PushEvent(&e);
        } else if (sscanf(l, "key %1900s", arg) == 1) {
            SDL_Event e; memset(&e, 0, sizeof e);
            e.type = SDL_KEYDOWN; e.key.keysym.sym = SDL_GetKeyFromName(arg); SDL_PushEvent(&e);
        } else if (!strncmp(l, "clip ", 5)) {
            SDL_SetClipboardText(l + 5);
        } else if (sscanf(l, "shot %1900s", arg) == 1) {
            int w, h; SDL_GetRendererOutputSize(r, &w, &h);
            SDL_Surface* s = SDL_CreateRGBSurface(0, w, h, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
            SDL_RenderReadPixels(r, NULL, s->format->format, s->pixels, s->pitch);
            SDL_SaveBMP(s, arg); SDL_FreeSurface(s);
        } else if (!strcmp(l, "quit")) {
            SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_QUIT; SDL_PushEvent(&e);
        }
    }
}
