#!/share/apps/python/anaconda/bin/python

import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

def make_plot(runs, scale):
    "Plot results of timing trials"
    df_basic = pd.DataFrame()
    for i in range(8):
        temp = pd.read_csv("basic/timing_" + scale + "_" + str(i + 1) + ".csv")
        df_basic = df_basic.append(temp, ignore_index = True)

    for arg in runs:
        df_arg = pd.DataFrame()
        for i in range(8):
            temp = pd.read_csv(str(arg) + "/timing_" + scale + "_" + str(i + 1) + ".csv")
            df_arg = df_arg.append(temp, ignore_index = True)

        plt.plot(df_arg.nthreads, df_basic.time / df_arg.time, label = 'version '+ str(arg))
        plt.title(scale + " scaling study")
        plt.xlabel('number of threads')
        plt.ylabel('speedup')

def show(runs):
    "Show plot of timing runs (for interactive use)"
    make_plot(runs)
    lgd = plt.legend(bbox_to_anchor=(1.05, 1), loc=2, borderaxespad=0.)
    plt.show()

def main(runs):
    "Show plot of timing runs (non-interactive)"
    make_plot(runs, "strong")
    lgd = plt.legend(bbox_to_anchor=(1.05, 1), loc=2, borderaxespad=0.)
    plt.savefig('timing_strong.pdf', bbox_extra_artists=(lgd,), bbox_inches='tight')
    plt.close("all")
    make_plot(runs, "weak")
    lgd = plt.legend(bbox_to_anchor=(1.05, 1), loc=2, borderaxespad=0.)
    plt.savefig('timing_weak.pdf', bbox_extra_artists=(lgd,), bbox_inches='tight')

if __name__ == "__main__":
    main(sys.argv[1:])
