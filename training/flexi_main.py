#!/usr/bin/env python
import argparse
import numpy as np
import utils as utils

from bo_only_learn_priority import learn_priority

def main(args):
    np.random.seed(args.seed)

    cfg = utils.setup(args)

    command = ['bash ./scripts/bench.sh -a -s {} -t {}'.format(args.skewness, args.nworkers)]
    learn_priority(command, cfg.get('log_directory'))    # , args.gx, args.tx


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)

    parser.add_argument('--base-log-dir', type=str, default='./training/bo-all',
                        help='model save location')
    parser.add_argument('--base-kid-dir', type=str, default='./training/bo',
                        help='kid policy save location')
    parser.add_argument('--expr-name', type=str, default='bo',
                        help='experiment name')
    parser.add_argument('--seed', help='RNG seed', type=int, default=42)

    # Experiment setting arguments
    parser.add_argument('--nworkers', type=int, default=16,
                        help='number of database workers')
    parser.add_argument('--skewness', type=float, default=0.8,
                        help='the workload skewness (YCSB)')

    main(parser.parse_args())
