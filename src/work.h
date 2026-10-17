/* Command line for the workloads: gen, net2asm, toyhash, verify, equiv, search, life. */
#ifndef ALPHA_WORK_H
#define ALPHA_WORK_H

/* Runs the command in argv[1] if it is a workload command; returns -1 if it is not one. */
int  work_main(int argc, char **argv);
void work_usage(void);

/* alpha cube (src/cubedemo.c) */
int  cmd_cube(const char *argv0, int argc, char **argv);

/* alpha beta (src/betacmd.c): the byte computer of step 8 */
int  cmd_beta(int argc, char **argv);

/* alpha super (src/supercmd.c): the superoptimizer */
int  cmd_super(int argc, char **argv);

/* alpha gamma (src/gammacmd.c): the world of step 11; argv0 finds lib/gamma.html for --serve */
int  cmd_gamma(const char *argv0, int argc, char **argv);

#endif
