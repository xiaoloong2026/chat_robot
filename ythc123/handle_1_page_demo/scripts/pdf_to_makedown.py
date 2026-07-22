#!/usr/bin/env python3

import os
import sys
import glob
import base64
import requests
from pdf2image import convert_from_path


# OCR服务地址
OCR_SERVER = "http://127.0.0.1:8081/v1/chat/completions"


# 当前脚本路径
SCRIPT_DIR = os.path.dirname(
    os.path.abspath(__file__)
)

# 项目根目录
ROOT_DIR = os.path.dirname(
    SCRIPT_DIR
)


# 中间文件目录
PNG_DIR = os.path.join(
    ROOT_DIR,
    "workspace",
    "png"
)

MD_DIR = os.path.join(
    ROOT_DIR,
    "workspace",
    "makedown"
)


os.makedirs(
    PNG_DIR,
    exist_ok=True
)

os.makedirs(
    MD_DIR,
    exist_ok=True
)



def pdf_to_png(pdf_path):

    # print("PDF转换PNG...")


    pages = convert_from_path(
        pdf_path,
        dpi=500
    )


    for i, page in enumerate(
        pages,
        start=1
    ):

        img_path=os.path.join(
            PNG_DIR,
            f"page_{i:03d}.png"
        )


        page.save(
            img_path,
            "PNG"
        )


        # print(
        #     "生成:",
        #     img_path
        # )


    return PNG_DIR



def image_to_markdown(img_path):


    # print(
    #     "OCR:",
    #     img_path
    # )


    with open(
        img_path,
        "rb"
    ) as f:

        img64=base64.b64encode(
            f.read()
        ).decode()



    prompt="""
你是专业文档OCR系统。

任务：
完整识别图片中的所有内容。

要求：
1. 不遗漏任何文字。
2. 保留原始阅读顺序。
3. 保留标题层级。
4. 保留编号。
5. 表格必须转换为Markdown表格。
6. 数学公式使用LaTeX。
7. 代码保持代码块格式。
8. 不进行总结。
9. 不解释图片内容。
10. 不添加不存在的信息。

输出完整Markdown文本。
"""


    data={

        "model":"glm-ocr",

        "messages":[
            {
                "role":"user",
                "content":[

                    {
                        "type":"text",
                        "text":prompt
                    },

                    {
                        "type":"image_url",
                        "image_url":{
                            "url":
                            "data:image/png;base64,"
                            +img64
                        }
                    }
                ]
            }
        ],

        "temperature":0.05,
        "max_tokens":4096
    }



    r=requests.post(
        OCR_SERVER,
        json=data,
        timeout=180
    )


    result=r.json()


    return result[
        "choices"
    ][0][
        "message"
    ][
        "content"
    ]



def main(pdf):


    # ======================
    # 1 PDF -> PNG
    # ======================

    img_dir=pdf_to_png(
        pdf
    )


    images=sorted(
        glob.glob(
            img_dir+"/*.png"
        )
    )


    # ======================
    # 2 PNG -> Markdown
    # ======================


    document_md=os.path.join(
        MD_DIR,
        "document.md"
    )


    with open(
        document_md,
        "w",
        encoding="utf-8"
    ) as document:


        for img in images:


            md=image_to_markdown(
                img
            )


            # 单页markdown
            page_name=os.path.splitext(
                os.path.basename(img)
            )[0]


            page_md=os.path.join(
                MD_DIR,
                page_name+".md"
            )


            with open(
                page_md,
                "w",
                encoding="utf-8"
            ) as f:

                f.write(md)



            # 合并总文档

            document.write(
                "\n\n"
            )

            document.write(
                f"# {page_name}\n\n"
            )

            document.write(
                md
            )

            document.write(
                "\n\n"
            )


            # print(
            #     "生成:",
            #     page_md
            # )



    # print("\n全部完成")
    # print(
    #     "Markdown:",
    #     document_md
    # )



if __name__=="__main__":


    if len(sys.argv)!=2:

        print(
            "用法:"
        )

        print(
            "python3 pdf_to_markdown.py xxx.pdf"
        )

        exit(1)


    main(
        sys.argv[1]
    )