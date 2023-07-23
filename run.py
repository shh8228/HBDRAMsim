import os
import argparse
import math
import openpyxl
import json

parser = argparse.ArgumentParser()
parser.add_argument("-p", default="traces", help="trace path")
parser.add_argument("-n", default="Sheet", help="sheet name")

def run(path, name):

    layer_dict = {"createQKV":'B', "QK":'C', "SV":'D', "L1":'F', "L2":'G'}
    if os.path.exists('result.xlsx'):
        wb = openpyxl.load_workbook('result.xlsx')
    else:
        wb = openpyxl.Workbook()

    fin = open("models", 'r')
    lines = fin.readlines()
    for line in lines:
        model, n_heads, d_head, n_layers  = line.strip().split(' ')

        sum_path = "traces/" + model + "/SUM"
        slist = sorted(os.listdir(sum_path))
        name = model + "_SUM"
        if name in wb.get_sheet_names():
            wb.remove_sheet(wb.get_sheet_by_name(name))

        wb.create_sheet(title=name)
        sh = wb.get_sheet_by_name(name)

        for i, s in enumerate(slist):
            sh['A'+str(i+1)] = s
            flist = sorted(os.listdir(sum_path + '/' + s))
            for j, f in enumerate(flist):
                print(i, j, f)
                os.system("echo " + f + " >> " + model + "_SUM_" + s + ".log")
                os.system("./build/dramsim3main configs/HBM2_8Gb_x128.ini -c 5000000 -t " + sum_path + '/' + s + '/' + f  + " >> " + model + "_SUM_" + s + ".log")

                with open('dramsim3.json') as df:
                    json_object = json.load(df)
                    cycles = json_object['0']['num_cycles']
                    if cycles == 5000000:
                        raise Exception("compute not finished!")
                    cycles *= int(n_layers)
                    if f == "createQKV":
                        sh['E' + str(i+1)] = cycles
                        cycles *= 3
                    elif f == "QK" or f == "SV":
                        cycles *= int(n_heads)
                    sh[layer_dict[f] + str(i+1)] = cycles

        gen_path = "traces/" + model + "/GEN"
        flist = sorted(os.listdir(gen_path))
        name = model + "_GEN"
        if name in wb.get_sheet_names():
            wb.remove_sheet(wb.get_sheet_by_name(name))

        wb.create_sheet(title=name)
        sh = wb.get_sheet_by_name(name)
        seq_lens = range(0, 11)
        for i, sl in enumerate(seq_lens):
            sh['A'+str(i+1)] = sl
        for i, f in enumerate(flist):
                print(i, f)
                os.system("echo " + f + " >> " + model + "_GEN.log")
                os.system("./build/dramsim3main configs/HBM2_8Gb_x128.ini -c 5000000 -t " + gen_path + '/' + f  + " >> " + model + "_GEN.log")

                with open('dramsim3.json') as df:
                    json_object = json.load(df)
                    cycles = json_object['0']['num_cycles']
                    if cycles == 5000000:
                        raise Exception("compute not finished!")
                    cycles *= int(n_layers)

                    for j, sl in enumerate(seq_lens):
                        if f == "createQKV":
                            sh['E' + str(j+1)] = cycles*sl
                            sh[layer_dict[f] + str(j+1)] = cycles*sl*3
                        elif "QK" in f or "SV" in f:
                            l, ii = f.split('_')
                            if int(ii) <= sl:
                                sh_ = sh[layer_dict[l] + str(int(ii))]
                                sh_ = sh_.value + cycles*int(n_heads)
                        else:
                            sh[layer_dict[f] + str(j+1)] = cycles*sl


        wb.save('result.xlsx')
    return


if __name__ == "__main__":
    args = parser.parse_args()
    run(args.p, args.n)
