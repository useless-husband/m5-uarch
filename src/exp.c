#include "exp.h"

const ua_experiment ua_experiments[] = {
    {"width", "pipeline width and execution-unit counts", ua_exp_width},
    {"window", "reorder buffer, register files, load/store queues (Wong's method)", ua_exp_window},
    {"elim", "move elimination and zero idioms", ua_exp_elim},
    {"fusion", "instruction pairs that execute as one", ua_exp_fusion},
    {"branch", "branch misprediction penalty", ua_exp_branch},
    {"cache", "L1/L2 capacity and load-to-use latency", ua_exp_cache},
    {"tlb", "data TLB reach", ua_exp_tlb},
    {"spec", "value and select speculation (load value prediction, csel)", ua_exp_spec},
};
const size_t ua_n_experiments = sizeof ua_experiments / sizeof ua_experiments[0];
