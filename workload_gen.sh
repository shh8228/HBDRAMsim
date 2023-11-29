#!/bin/zsh

for ((var=1; var<=1024; var *= 2));
do
    python gen_workload_gpt_sum.py -s $var
done

python gen_workload_gpt_gen.py -s 1 -e 1024 -l all
