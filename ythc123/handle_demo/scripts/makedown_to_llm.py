#!/usr/bin/env python3

import os
import requests


LLM_SERVER = "http://127.0.0.1:8082/v1/chat/completions"



def load_markdown():

    SCRIPT_DIR = os.path.dirname(
        os.path.abspath(__file__)
    )

    ROOT_DIR = os.path.dirname(
        SCRIPT_DIR
    )


    md_file = os.path.join(
        ROOT_DIR,
        "workspace",
        "makedown",
        "document.md"
    )


    if not os.path.exists(md_file):

        return ""


    with open(
        md_file,
        "r",
        encoding="utf-8"
    ) as f:

        markdown = f.read()


    return markdown




def main(user_prompt):


    markdown = load_markdown()


    prompt = f"""

{user_prompt}


参考PDF文档：

{markdown}

"""


    data = {

        "model": "qwen",

        "messages": [

            {
                "role": "user",
                "content": prompt
            }

        ],


        "temperature": 0.2,


        "max_tokens": 1024,


        "chat_template_kwargs": {

            "enable_thinking": False

        }

    }



    try:

        r = requests.post(
            LLM_SERVER,
            json=data,
            timeout=120
        )


        result = r.json()


        message = result["choices"][0]["message"]


        # 正常回答
        if message.get("content"):

            return message["content"]


        # 防止thinking模式导致空
        if message.get("reasoning_content"):

            return message["reasoning_content"]


        return ""


    except Exception as e:

        print("LLM请求失败:")
        print(e)

        return ""