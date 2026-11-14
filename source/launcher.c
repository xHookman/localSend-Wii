/*
 * launcher.c - lance un autre homebrew (.dol / .elf) sans repasser par le HBC
 * Meme methode que USB Loader GX : DOL en MEM2, petit chargeur en 0x93000000.
 */
#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include <ogc/irq.h>
#include <network.h>
#include <fat.h>
#include <wiiuse/wpad.h>
#include "launcher.h"
#include "app_booter.h"

#define EXECUTE_ADDR ((u8 *)0x92000000)
#define BOOTER_ADDR  ((u8 *)0x93000000)
#define ARGS_ADDR    ((u8 *)0x93200000)
#define MAX_SIZE     (16 * 1024 * 1024)

static int find_first(const char *forced, const char *const *paths, char *out, int max) {
    FILE *f;
    if (forced && forced[0] && (f = fopen(forced, "rb"))) {
        fclose(f);
        snprintf(out, max, "%s", forced);
        return 1;
    }
    for (int i = 0; paths[i]; i++) {
        if ((f = fopen(paths[i], "rb"))) {
            fclose(f);
            snprintf(out, max, "%s", paths[i]);
            return 1;
        }
    }
    return 0;
}

int launcher_find(const char *forced, char *out, int max) {
    static const char *const paths[] = {
        "sd:/apps/WiiXplorer/boot.dol", "sd:/apps/wiixplorer/boot.dol",
        "sd:/apps/WiiXplorer/boot.elf", "usb:/apps/WiiXplorer/boot.dol",
        "usb:/apps/WiiXplorer/boot.elf", NULL };
    return find_first(forced, paths, out, max);
}

int launcher_find_yawmme(char *out, int max) {
    static const char *const paths[] = {
        "sd:/apps/yawmME/boot.dol", "sd:/apps/yawmme/boot.dol", "sd:/apps/YAWMME/boot.dol",
        "sd:/apps/yawmME/boot.elf", "usb:/apps/yawmME/boot.dol", "usb:/apps/yawmME/boot.elf", NULL };
    return find_first(NULL, paths, out, max);
}

const char *launcher_run(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "fichier introuvable";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x100 || size > MAX_SIZE) { fclose(f); return "taille de fichier invalide"; }
    if (fread(EXECUTE_ADDR, 1, size, f) != (size_t)size) { fclose(f); return "lecture impossible"; }
    fclose(f);
    DCFlushRange(EXECUTE_ADDR, size);

    /* argv[0] = chemin du programme (sert a retrouver sa configuration) */
    struct __argv *args = (struct __argv *)ARGS_ADDR;
    char *cmd = (char *)ARGS_ADDR + sizeof(struct __argv);
    int len = (int)strlen(path) + 1;
    memset(args, 0, sizeof(*args));
    memcpy(cmd, path, len);
    cmd[len] = 0;
    args->argvMagic = ARGV_MAGIC;
    args->commandLine = cmd;
    args->length = len + 1;
    args->argc = 1;
    args->argv = &args->commandLine;
    args->endARGV = args->argv + 1;
    DCFlushRange(ARGS_ADDR, sizeof(struct __argv) + len + 1);

    memcpy(BOOTER_ADDR, app_booter_bin, sizeof(app_booter_bin));
    DCFlushRange(BOOTER_ADDR, sizeof(app_booter_bin));
    ICInvalidateRange(BOOTER_ADDR, sizeof(app_booter_bin));

    /* Arret propre : SD/USB, manettes, reseau, puis systeme */
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fatUnmount("sd:");
    fatUnmount("usb:");
    WPAD_Shutdown();
    net_deinit();
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);

    IRQ_Disable();
    ((void (*)(void))BOOTER_ADDR)();
    return "le lancement a \xC3\xA9" "chou\xC3\xA9";   /* jamais atteint */
}
