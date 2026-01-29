#!/usr/bin/env python3
"""
Analyze and visualize PostgreSQL VACUUM benchmark results
Comparing ART vs QuART performance
"""

import pandas as pd
import sys
import glob
import os

def find_latest_results(results_dir='benchmark_results'):
    """Find the most recent benchmark CSV file"""
    pattern = os.path.join(results_dir, 'benchmark_*.csv')
    files = glob.glob(pattern)
    if not files:
        print(f"No benchmark files found in {results_dir}/")
        sys.exit(1)
    return max(files, key=os.path.getctime)

def load_and_analyze(csv_file):
    """Load benchmark data and compute statistics"""
    df = pd.read_csv(csv_file)
    
    # Convert dead_tuple_bytes to numeric (handle N/A values)
    df['dead_tuple_bytes'] = pd.to_numeric(df['dead_tuple_bytes'], errors='coerce')
    
    # Group by configuration
    summary = df.groupby('config').agg({
        'elapsed_seconds': ['mean', 'std', 'min', 'max'],
        'index_vacuum_count': ['mean', 'std'],
        'cpu_user': ['mean', 'std'],
        'cpu_system': ['mean', 'std'],
        'dead_tuple_bytes': ['mean', 'std']
    }).round(2)
    
    return df, summary

def plot_results(df, output_dir='benchmark_results'):
    """Create visualization plots"""
    try:
        import matplotlib.pyplot as plt
        import seaborn as sns
    except ImportError:
        print("\nWarning: matplotlib and seaborn not installed. Skipping plots.")
        print("Install with: pip3 install matplotlib seaborn")
        return
    
    sns.set_style("whitegrid")
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # Plot 1: Elapsed Time Comparison
    ax1 = axes[0, 0]
    sns.boxplot(data=df, x='config', y='elapsed_seconds', ax=ax1)
    ax1.set_title('VACUUM Elapsed Time (seconds)', fontsize=12, fontweight='bold')
    ax1.set_xlabel('Configuration')
    ax1.set_ylabel('Time (seconds)')
    
    # Add mean values as text
    means = df.groupby('config')['elapsed_seconds'].mean()
    for i, (config, mean) in enumerate(means.items()):
        ax1.text(i, mean, f'{mean:.1f}s', ha='center', va='bottom', fontweight='bold')
    
    # Plot 2: Index Vacuum Count
    ax2 = axes[0, 1]
    sns.barplot(data=df, x='config', y='index_vacuum_count', ax=ax2, ci='sd')
    ax2.set_title('Index Vacuum Count', fontsize=12, fontweight='bold')
    ax2.set_xlabel('Configuration')
    ax2.set_ylabel('Number of Index Scans')
    
    # Plot 3: CPU Time Breakdown
    ax3 = axes[1, 0]
    cpu_data = df.groupby('config')[['cpu_user', 'cpu_system']].mean()
    cpu_data.plot(kind='bar', stacked=True, ax=ax3, color=['#2ecc71', '#e74c3c'])
    ax3.set_title('CPU Time Breakdown', fontsize=12, fontweight='bold')
    ax3.set_xlabel('Configuration')
    ax3.set_ylabel('CPU Time (seconds)')
    ax3.legend(['User', 'System'])
    ax3.set_xticklabels(ax3.get_xticklabels(), rotation=0)
    
    # Plot 4: Performance Improvement
    ax4 = axes[1, 1]
    if 'ART' in df['config'].values and 'QuART' in df['config'].values:
        art_mean = df[df['config'] == 'ART']['elapsed_seconds'].mean()
        quart_mean = df[df['config'] == 'QuART']['elapsed_seconds'].mean()
        improvement = ((art_mean - quart_mean) / art_mean) * 100
        
        bars = ax4.bar(['ART', 'QuART'], [0, improvement], color=['#95a5a6', '#27ae60'])
        ax4.set_title('Performance Improvement (%)', fontsize=12, fontweight='bold')
        ax4.set_ylabel('Improvement over ART (%)')
        ax4.axhline(y=0, color='black', linestyle='-', linewidth=0.5)
        
        # Add value labels
        for bar in bars:
            height = bar.get_height()
            if height != 0:
                ax4.text(bar.get_x() + bar.get_width()/2., height,
                        f'{height:+.1f}%',
                        ha='center', va='bottom' if height > 0 else 'top',
                        fontweight='bold', fontsize=11)
    
    plt.tight_layout()
    
    # Save plot
    plot_file = os.path.join(output_dir, f'benchmark_plot_{pd.Timestamp.now().strftime("%Y%m%d_%H%M%S")}.png')
    plt.savefig(plot_file, dpi=300, bbox_inches='tight')
    print(f"\nPlot saved to: {plot_file}")
    
    plt.show()

def print_summary(df, summary):
    """Print detailed summary to console"""
    print("\n" + "="*70)
    print("BENCHMARK SUMMARY")
    print("="*70)
    print(f"\nTotal runs per configuration: {df.groupby('config').size().values[0]}")
    print(f"Number of rows: {df['num_rows'].iloc[0]:,}")
    print(f"Maintenance work mem: {df['maintenance_work_mem'].iloc[0]}")
    
    print("\n" + "-"*70)
    print("DETAILED STATISTICS")
    print("-"*70)
    print(summary.to_string())
    
    # Calculate and print improvement
    if 'ART' in df['config'].values and 'QuART' in df['config'].values:
        art_elapsed = df[df['config'] == 'ART']['elapsed_seconds'].mean()
        quart_elapsed = df[df['config'] == 'QuART']['elapsed_seconds'].mean()
        improvement = ((art_elapsed - quart_elapsed) / art_elapsed) * 100
        
        art_scans = df[df['config'] == 'ART']['index_vacuum_count'].mean()
        quart_scans = df[df['config'] == 'QuART']['index_vacuum_count'].mean()
        scan_reduction = art_scans - quart_scans
        
        print("\n" + "="*70)
        print("KEY FINDINGS")
        print("="*70)
        print(f"  Time improvement: {improvement:+.1f}%")
        print(f"  ART:   {art_elapsed:.2f}s average")
        print(f"  QuART: {quart_elapsed:.2f}s average")
        print(f"\n  Index scan reduction: {scan_reduction:.1f} fewer scans")
        print(f"  ART:   {art_scans:.1f} scans")
        print(f"  QuART: {quart_scans:.1f} scans")
        print("="*70 + "\n")

def main():
    """Main analysis function"""
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]
    else:
        csv_file = find_latest_results()
    
    print(f"Analyzing: {csv_file}")
    
    df, summary = load_and_analyze(csv_file)
    print_summary(df, summary)
    
    # Create plots
    try:
        plot_results(df, os.path.dirname(csv_file))
    except Exception as e:
        print(f"\nWarning: Could not create plots: {e}")
        print("Make sure matplotlib and seaborn are installed:")
        print("  pip3 install matplotlib seaborn pandas")

if __name__ == '__main__':
    main()
