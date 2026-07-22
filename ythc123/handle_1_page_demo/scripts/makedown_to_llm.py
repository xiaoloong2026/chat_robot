#!/usr/bin/env python3

import os
import requests


LLM_SERVER="http://127.0.0.1:8082/v1/chat/completions"


def load_markdown():

    SCRIPT_DIR=os.path.dirname(
        os.path.abspath(__file__)
    )

    ROOT_DIR=os.path.dirname(
        SCRIPT_DIR
    )


    md_file=os.path.join(
        ROOT_DIR,
        "workspace",
        "makedown",
        "document.md"
    )


    with open(
        md_file,
        "r",
        encoding="utf-8"
    ) as f:

        markdown=f.read()


    # print("Markdown加载完成")
    # print("长度:",len(markdown))


    return markdown



def chat_loop(markdown):


    while True:


        question=input(
            "\n请输入问题(q退出): "
        )


        if question=="q":
            break



        prompt=f"""

你是一个文档分析助手。

请根据下面Markdown文档回答问题。

要求：
1. 只能根据文档回答。
2. 不要编造。
3. 找不到内容请说明。


========
文档:
========

{markdown}


========
问题:
========

{question}

"""


        data={

            "model":"qwen",

            "messages":[
                {
                    "role":"user",
                    "content":prompt
                }
            ],

            "temperature":0.2,

            "max_tokens":1024
        }



        r=requests.post(
            LLM_SERVER,
            json=data,
            timeout=120
        )


        result=r.json()


        print("\n回答:")

        print(
            result["choices"][0]
            ["message"]
            ["content"]
        )



def main():


    markdown=load_markdown()

    chat_loop(markdown)



if __name__=="__main__":

    main()