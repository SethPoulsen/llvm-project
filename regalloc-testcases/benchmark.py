#!/usr/bin/env python3.8

import asyncio
from glob import glob
import subprocess as sp
import sys

register_allocators = ["ranaive", "rass", "basic", "greedy", "fast", "pbqp"]
test_runs = 5
fileNames = [fileName[:-2] for fileName in glob('*.c')]

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
    await async_run(["../build/bin/llc",
                     fileName + ".bc",
                     "-regalloc=" + allocator,
                     '-o',
                     asm_name],
                    capture_output=True, check=True)
    # assembly -> binary
    await async_run(["clang", asm_name, "-lm", '-o', bin_name],
                    capture_output=True, check=True)
    times = await asyncio.gather(*[benchmark_allocator_once(bin_name)
                                   for _ in range(0, test_runs)])
    return average(times)

def average(times):
    return sum(times) / len(times)

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
    output = [['test-case', *register_allocators]]
    results = asyncio.run(benchmark(fileNames))
    for fileName, results in zip(fileNames, results):
        results = [round(result, 3) for result in results]
        output.append([fileName, *results])
    table(output)
