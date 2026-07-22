#!/usr/bin/env python3

import sys


from scripts.pdf_to_makedown import main as pdf_to_md

from scripts.makedown_to_llm import main as llm_chat



def main(pdf):


    # print("===================")
    # print("开始PDF OCR")
    # print("===================")


    print("===================")
    print("已加载PDF")
    print("===================")

    # 第一阶段
    pdf_to_md(pdf)



    print("===================")
    print("进入LLM问答")
    print("===================")


    # 第二阶段
    llm_chat()



if __name__=="__main__":


    if len(sys.argv)!=2:

        print(
            "用法:"
        )

        print(
            "python3 main.py xxx.pdf"
        )

        exit(1)


    main(
        sys.argv[1]
    )