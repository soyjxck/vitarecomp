/* host_main.h — the ready-made host, for a program of its own.
 *
 * host.h is what the runtime asks of whatever hosts it; host/ answers it:
 * a window or none, scripted input, frames to files, the traces. vita_host
 * is that with main() in front and nothing else.
 *
 * A game's program is the same thing behind a main() of its own, one that
 * knows where the title's files are, reads the player's settings, sets the
 * title up the first time, and then calls this with the arguments it has
 * made up: the module, the mounts, --window, --title. In CMake:
 *
 *     add_subdirectory(vitarecomp)
 *     vitarecomp_add_game(my-game RECOMP_DIR <where armrecomp emit wrote> SOURCES src/main.c ...)
 *
 * The arguments are vita_host's own (host/host.c lists them). It returns
 * only for a command line it cannot read, or when module_start returns; a
 * title that runs until it is closed ends the process from inside.
 */

#ifndef VITARECOMP_HOST_MAIN_H
#define VITARECOMP_HOST_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

int vita_host_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_HOST_MAIN_H */
