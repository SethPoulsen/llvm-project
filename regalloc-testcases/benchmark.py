#!/usr/bin/python3.8

import os 
import subprocess as sp
from os import path

register_allocators = ["ranaive", "rass", "basic", "greedy", "fast", "pbqp"]
test_runs = 5
fileNames = [path.basename(x)[:-2] for x in os.listdir("./") if x[-2:] == ".c"]
# print(fileNames)
# fileNames.append("nonexistent_file")
def benchmarkFile(fileName): 
    # print(fileName)
    result = sp.run(["clang", "-c", "-emit-llvm", fileName + ".c", "-g"])
    if result.returncode != 0: 
        raise Exception("Failed to convert " + fileName + " to bitcode")

    averages = []
    for allocator in register_allocators:
        result = sp.run(["../build/bin/llc", fileName + ".bc", "-regalloc=" + allocator])
        if result.returncode != 0:
            raise Exception("Failed to generate assembly.")

        # assembly -> binary
        result = sp.run(["clang", fileName + ".s", "-lm"])
        if result.returncode != 0:
            raise Exception("Failed to create binary from assembly.")

        times = []
        for i in range(0, test_runs):
            args = ["/usr/bin/time", r'--format=%U, %S, %E', "./a.out"]
            result = sp.run(args, capture_output=True, check=True)
            time = result.stderr.decode("utf-8").split(",")[0]
            times.append(float(time))
        averages.append(sum(times) / len(times))

    return fileName, averages
    

output = "test-case, " + ", ".join(register_allocators) + "\n"

for fileName, results in map(benchmarkFile, fileNames): 
    output += fileName + ", " + ", ".join(map(str, results)) + "\n"

print(output)
