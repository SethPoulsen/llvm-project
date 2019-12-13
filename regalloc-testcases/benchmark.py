#!/usr/bin/env python3

import asyncio
from glob import glob
import collections
import subprocess as sp
import math
import sys
import re
import csv

register_allocators = ["ranaive", "rass", "basic", "greedy", "fast", "pbqp"]
# register_allocators = ["ranaive"]
test_runs = 14
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
    await async_run(["clang", "-c", "-emit-llvm", fileName + ".c", '-O0'],
                    capture_output=True, check=True)
    return await asyncio.gather(*[benchmark_allocator(fileName, allocator)
                                  for allocator in register_allocators])

async def benchmark_allocator(fileName, allocator):
    asm_name = f'./{fileName}_{allocator}.s'
    bin_name = f'./{fileName}_{allocator}.exe'
    stats = await async_run([
        "../build/bin/llc",
        fileName + ".bc",
        "-regalloc=" + allocator,
        '-O=3',
        '-o',
        asm_name,
        "-stats"
    ], capture_output=True, check=True)
    match = re.search(spills_regex, stats.stderr.decode("utf-8"))
    spills = int(match.group(1)) if match else 0

    # assembly -> binary
    await async_run(["clang", asm_name, "-lm", '-o', bin_name, '-O3'],
                    capture_output=True, check=True)

    try:
        times = await asyncio.gather(*[benchmark_allocator_once(bin_name)
                                       for _ in range(0, test_runs)])
    except RuntimeError:
        return [0, 0], spills
    else:
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

def mean(lst):
    return sum(lst) / len(lst)

def std(lst, ddof=1):
    avg = mean(lst)
    return math.sqrt(sum((x - avg)**2 for x in lst) / (len(lst) - ddof))

if __name__ == '__main__':
    all_results = asyncio.run(benchmark(fileNames))

    with open(f'time_mean.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow([''] + register_allocators)
        for fileName, results in zip(fileNames, all_results):
            writer.writerow([fileName] + [mean(result[0]) for result in results])
            
    with open(f'time_std.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow([''] + register_allocators)
        for fileName, results in zip(fileNames, all_results):
            writer.writerow([fileName] + [std(result[0]) for result in results])

    with open(f'spills.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow([''] + register_allocators)
        for fileName, results in zip(fileNames, all_results):
            writer.writerow([fileName] + [result[1] for result in results])
