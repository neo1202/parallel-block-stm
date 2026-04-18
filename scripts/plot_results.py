#!/usr/bin/env python3
"""Read benchmark CSV and plot scaling curves / contention sweeps."""

import csv
import sys
import os

try:
    import matplotlib.pyplot as plt
except ImportError:
    print("Error: matplotlib is not installed.")
    print("Please install it by running: pip3 install matplotlib")
    sys.exit(1)

def read_csv(file_path):
    data = []
    with open(file_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            data.append(row)
    return data

def plot_scaling(csv_file, out_dir):
    data = read_csv(csv_file)
    if not data:
        return
    threads = [int(row['threads']) for row in data]
    throughputs = [float(row['throughput']) for row in data]
    
    # Normalizing to speedup
    t1_throughput = throughputs[threads.index(1)] if 1 in threads else throughputs[0]
    speedups = [t / t1_throughput for t in throughputs]

    # Plot Speedup
    plt.figure(figsize=(8, 5))
    plt.plot(threads, speedups, marker='o', linestyle='-', color='b', label='Parallel Block-STM')
    plt.plot(threads, threads, linestyle='--', color='gray', label='Ideal Scaling')
    plt.title('Scaling Benchmark (Speedup)')
    plt.xlabel('Number of Threads')
    plt.ylabel('Speedup over Sequential (1 Thread)')
    plt.xticks(threads)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'scaling_speedup.png'))
    plt.close()

    # Plot Throughput
    plt.figure(figsize=(8, 5))
    plt.plot(threads, throughputs, marker='o', linestyle='-', color='g', label='Throughput')
    plt.title('Scaling Benchmark (Throughput)')
    plt.xlabel('Number of Threads')
    plt.ylabel('Throughput (TPS)')
    plt.xticks(threads)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'scaling_throughput.png'))
    plt.close()

def plot_sweep(csv_file, out_dir):
    data = read_csv(csv_file)
    if not data:
        return
        
    accounts_data = {}
    for row in data:
        acc = int(row['total_accounts'])
        if acc not in accounts_data:
            accounts_data[acc] = {'threads': [], 'throughputs': []}
        accounts_data[acc]['threads'].append(int(row['threads']))
        accounts_data[acc]['throughputs'].append(float(row['throughput']))

    labels = {
        5000: 'Low Contention (5000 acc)',
        500: 'Mid Contention (500 acc)',
        10: 'High Contention (10 acc)'
    }
    
    # Plot Speedup
    plt.figure(figsize=(8, 5))
    for acc, group in sorted(accounts_data.items(), reverse=True):
        threads = group['threads']
        throughputs = group['throughputs']
        
        sorted_pairs = sorted(zip(threads, throughputs))
        threads = [p[0] for p in sorted_pairs]
        throughputs = [p[1] for p in sorted_pairs]

        t1_throughput = throughputs[threads.index(1)] if 1 in threads else throughputs[0]
        speedups = [t / t1_throughput for t in throughputs]
        
        label = labels.get(acc, f'{acc} accounts')
        plt.plot(threads, speedups, marker='o', linestyle='-', label=label)

    plt.plot(threads, threads, linestyle='--', color='gray', label='Ideal Scaling')
    plt.title('Contention Sweep Benchmark (Speedup)')
    plt.xlabel('Number of Threads')
    plt.ylabel('Speedup over Sequential (1 Thread)')
    plt.xticks(threads)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'sweep_speedup.png'))
    plt.close()

    # Plot Throughput
    plt.figure(figsize=(8, 5))
    for acc, group in sorted(accounts_data.items(), reverse=True):
        threads = group['threads']
        throughputs = group['throughputs']
        
        sorted_pairs = sorted(zip(threads, throughputs))
        threads = [p[0] for p in sorted_pairs]
        throughputs = [p[1] for p in sorted_pairs]
        
        label = labels.get(acc, f'{acc} accounts')
        plt.plot(threads, throughputs, marker='o', linestyle='-', label=label)

    plt.title('Contention Sweep Benchmark (Throughput)')
    plt.xlabel('Number of Threads')
    plt.ylabel('Throughput (TPS)')
    plt.xticks(threads)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'sweep_throughput.png'))
    plt.close()

def plot_heavy(csv_file, out_dir):
    data = read_csv(csv_file)
    if not data:
        return
    threads = [int(row['threads']) for row in data]
    throughputs = [float(row['throughput']) for row in data]
    
    # Normalizing to speedup
    t1_throughput = throughputs[threads.index(1)] if 1 in threads else throughputs[0]
    speedups = [t / t1_throughput for t in throughputs]

    # Plot Speedup
    plt.figure(figsize=(8, 5))
    plt.plot(threads, speedups, marker='o', linestyle='-', color='r', label='Heavy Workload (VM Sim)')
    plt.plot(threads, threads, linestyle='--', color='gray', label='Ideal Scaling')
    plt.title('Heavy Workload Benchmark (Speedup)')
    plt.xlabel('Number of Threads')
    plt.ylabel('Speedup over Sequential (1 Thread)')
    plt.xticks(threads)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'heavy_speedup.png'))
    plt.close()

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 plot_results.py <csv_file1> [csv_file2...] <out_dir>")
        sys.exit(1)
        
    out_dir = sys.argv[-1]
    csv_files = sys.argv[1:-1]
    
    if not os.path.exists(out_dir):
        os.makedirs(out_dir)
        
    print(f"Generating plots in {out_dir} ...")
    for f in csv_files:
        if not os.path.exists(f):
            print(f"Warning: {f} not found, skipping.")
            continue
            
        if 'scaling' in os.path.basename(f):
            plot_scaling(f, out_dir)
        elif 'sweep' in os.path.basename(f):
            plot_sweep(f, out_dir)
        elif 'heavy' in os.path.basename(f):
            plot_heavy(f, out_dir)
