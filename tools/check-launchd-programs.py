#!/usr/bin/env python3
"""
check-launchd-programs.py: every launchd job in an extracted iOS root against the program it names.

usage: check-launchd-programs.py ROOTFS [--slt ROOTFS.SLT] [--dirs a,b,c] [--service NAME]

For each job property list, the program launchd would spawn is ProgramArguments[0], or Program when
there is no argument list, or BundleProgram for a job that names a program inside a bundle. Each of
those is checked twice: is the file in ROOTFS, and is it in the technical listing of the original
root filesystem image (ROOTFS.SLT, the output of `7z l -slt`)? The two answers separate the cases
that look alike from outside:

  present         the file is in the extracted root
  not extracted   the image has it, the extraction left it out: a gap in the extraction rules
  cache only      the image does not have it at all, so the program is only in the dyld shared
                  cache and something has to put it on disk before launchd can spawn it
  no program      the job names no program here (an XPC service or a program inside an app)
  not absolute    the program is a bare name launchd looks up in PATH (reported, not checked)

One line per job, tab separated, and a count per case at the end. Nothing is written anywhere.

--service NAME answers the other question: which job publishes that Mach service. It reads every
job's MachServices, because a service is not named after the job that provides it -- dasd publishes
com.apple.duetactivityscheduler, contextstored publishes com.apple.coreduetd.context, and neither
job is called after the service.
"""
import argparse
import os
import plistlib
import sys

CASES = ('present', 'not extracted', 'cache only', 'no program', 'not absolute', 'no plist')


def program_of(job):
    arguments = job.get('ProgramArguments')
    if arguments:
        return arguments[0]
    for key in ('Program', 'BundleProgram'):
        if job.get(key):
            return job[key]
    return None


def slt_paths(path):
    """The Path entries of a `7z l -slt` listing, without leading slashes."""
    paths = set()
    if path is None:
        return paths
    with open(path, 'r', errors='replace') as handle:
        for line in handle:
            if line.startswith('Path = '):
                paths.add(line[len('Path = '):].strip().lstrip('/'))
    return paths


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument('rootfs')
    parser.add_argument('--slt')
    parser.add_argument('--dirs', default='System/Library/LaunchDaemons,System/Library/LaunchAgents,'
                                          'System/Library/LaunchAngels')
    parser.add_argument('--service')
    args = parser.parse_args(argv[1:])

    listing = slt_paths(args.slt)
    rows, services = [], []
    for directory in args.dirs.split(','):
        full = os.path.join(args.rootfs, directory)
        if not os.path.isdir(full):
            rows.append((directory, '(no such directory)', 'no plist', ''))
            continue
        for name in sorted(os.listdir(full)):
            if not name.endswith('.plist'):
                rows.append((directory + '/' + name, '', 'no plist', ''))
                continue
            path = os.path.join(full, name)
            try:
                with open(path, 'rb') as handle:
                    job = plistlib.load(handle)
            except Exception as error:                  # a plist we cannot read is its own finding
                rows.append((directory + '/' + name, '', 'no plist', str(error)))
                continue
            for service in (job.get('MachServices') or {}):
                services.append((service, directory + '/' + name, program_of(job)))
            program = program_of(job)
            if program is None:
                rows.append((directory + '/' + name, '', 'no program', ''))
            elif not program.startswith('/'):
                rows.append((directory + '/' + name, program, 'not absolute', ''))
            elif os.path.exists(os.path.join(args.rootfs, program.lstrip('/'))):
                rows.append((directory + '/' + name, program, 'present', ''))
            elif program.lstrip('/') in listing:
                rows.append((directory + '/' + name, program, 'not extracted', ''))
            else:
                rows.append((directory + '/' + name, program, 'cache only', ''))

    if args.service is not None:
        found = [row for row in services if row[0] == args.service]
        for service, job, program in found:
            case = [row[2] for row in rows if row[0] == job]
            print('%s\t%s\t%s\t%s' % (service, job, program, case[0] if case else 'unknown'))
        if not found:
            print('%s\tnone\t(no job in the directories scanned publishes it)\t' % args.service)
        return 0

    for row in rows:
        print('\t'.join(row))
    print()
    for case in CASES:
        print('%s\t%d' % (case, sum(1 for row in rows if row[2] == case)))
    print('total\t%d' % len(rows))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))