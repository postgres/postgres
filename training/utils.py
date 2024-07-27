#!/usr/bin/env python

import datetime
import os

def save_arg_dict(d, base_dir='./', filename='args.txt', log=True):
    def _format_value(v):
        if isinstance(v, float):
            return '%.8f' % v
        elif isinstance(v, int):
            return '%d' % v
        else:
            return '%s' % str(v)

    with open(os.path.join(base_dir, filename), 'w') as f:
        for k, v in d.items():
            f.write('%s\t%s\n' % (k, _format_value(v)))
    if log:
        print('Saved settings to %s' % os.path.join(base_dir, filename))


def mkdir_p(path, log=True):
    import errno
    try:
        os.makedirs(path)
    except OSError as exc:
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else:
            raise
    if log:
        print('Created directory %s' % path)


def date_filename(base_dir='./', prefix=''):
    dt = datetime.datetime.now()
    return os.path.join(base_dir, '{}{}_{:02d}-{:02d}-{:02d}'.format(
        prefix, dt.date(), dt.hour, dt.minute, dt.second))


def setup(args):
    """Boilerplate setup, returning dict of configured items."""
    # log directory
    log_directory = date_filename(args.base_log_dir, args.expr_name)
    mkdir_p(log_directory)
    save_arg_dict(args.__dict__, base_dir=log_directory)
    # kid directory
    return dict(log_directory=log_directory, kid_directory=args.base_kid_dir)
