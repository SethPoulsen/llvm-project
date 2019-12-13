#!/usr/bin/env python3.7

import asyncio
from glob import glob
import collections
import subprocess as sp
import numpy as np
import sys
import re
import pandas as pd

# register_allocators = ["ranaive"]
register_allocators = ["ranaive", "rass", "basic", "greedy", "fast", "pbqp"]
test_runs = 5
fileNames = [fileName[:-2] for fileName in glob('*.c')]
# fileNames = ['quadratic']
spills_regex = r"\s*(\d+)\ regalloc\s+-\ Number of spills inserted"

async def async_run(cmd, capture_output=False, check=False):
    '''this is a clone of subprocess.run that is async'''
    stdout = asyncio.subprocess.PIPE if capture_output else None
    stderr = stdout
    proc = await asyncio.create_subprocess_exec(*cmd, stdout=stdout, stderr=stderr)
    stdout, stderr = await proc.communicate()
    if check:
        if proc.returncode != 0:
            if stderr:
                sys.stderr.write(stderr.decode())
            raise RuntimeError(f'{cmd!r} returned non-zero ({proc.returncode})')
    return sp.CompletedProcess(cmd, proc.returncode,
                               stdout=stdout, stderr=stderr)

async def benchmark(fileNames):
    return await asyncio.gather(*map(benchmark_file, fileNames))

async def benchmark_file(fileName):
    await async_run(["clang", "-c", "-emit-llvm", fileName + ".c",],
                    capture_output=True, check=True)
    return await asyncio.gather(*[benchmark_allocator(fileName, allocator)
                                  for allocator in register_allocators])

async def benchmark_allocator(fileName, allocator):
    asm_name = f'./{fileName}_{allocator}.s'
    bin_name = f'./{fileName}_{allocator}.exe'
    stats = await async_run(["../build/bin/llc",
                     fileName + ".bc",
                     "-regalloc=" + allocator,
                     '-o',
                     asm_name,
                     "-stats"],
                    capture_output=True, check=True)
    match = re.search(spills_regex, stats.stderr.decode("utf-8"))
    spills = match.group(1) if match else 0

    # assembly -> binary
    await async_run(["clang", asm_name, "-lm", '-o', bin_name],
                    capture_output=True, check=True)
    times = await asyncio.gather(*[benchmark_allocator_once(bin_name)
                                   for _ in range(0, test_runs)])
    return times, spills

async def benchmark_allocator_once(bin_name):
    args = ["/usr/bin/time", r'--format=%U,%S,%E', bin_name]
    result = await async_run(args, capture_output=True, check=True)
    time = float(result.stderr.decode("utf-8").split(",")[0])
    return time

def table(lists, size=15):
    row_format = f'{{:>{size}}}' * len(lists[0])
    for list_ in lists:
        print(row_format.format(*list_))

if __name__ == '__main__':
    time_std = collections.defaultdict(list)
    time_mean = collections.defaultdict(list)
    spills = collections.defaultdict(list)
    results = asyncio.run(benchmark(fileNames))
    for fileName, results in zip(fileNames, results):
        for allocator in register_allocators:
            time_mean[allocator].append(np.mean(result[0]))
            time_std[allocator].append(np.std(result[0], ddof=1))
            spills[allocator].append(result[1])

    time_mean = pd.DataFrame(data=time_mean, index=pd.Index(fileNames))
    print(time_mean)
    time_mean.to_csv('time_mean.csv')

    time_std = pd.DataFrame(data=time_std, index=pd.Index(fileNames))
    print(time_std)
    time_std.to_csv('time_std.csv')

    time_spills = pd.DataFrame(data=spills, index=pd.Index(fileNames))
    print(time_spills)
    time_spills.to_csv('spills.csv')
