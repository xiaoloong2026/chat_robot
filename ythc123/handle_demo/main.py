#!/usr/bin/env python3

import sys

from scripts.pdf_to_makedown import main as pdf_to_md
from scripts.makedown_to_llm import main as llm_chat



def build_health_prompt(user_input):

    prompt = f"""
你是一名医疗风险辅助分析AI。

任务：
根据用户提供的健康检查信息，分析可能存在的健康风险。

要求：
1. 只输出最终分析结果，不输出思考过程。
2. 不能替代医生诊断。
3. 不编造用户未提供的信息。
4. 根据医学指标组合进行判断。
5. 输出简洁、专业。


用户健康信息：

{user_input}


请严格按照以下格式输出：


【异常指标】

列出关键异常指标：
- 指标名称
- 检测结果
- 异常原因


【风险判断】

根据指标组合分析可能存在的问题。


【疾病匹配】

判断是否符合重大疾病风险。

格式：

符合/不符合「疾病名称」

判断依据：
xxx


【风险等级】

低风险 / 中风险 / 高风险

原因：
xxx


【处理建议】

给出下一步建议：
- 是否需要立即就医
- 建议进一步检查项目


"""

    return prompt




def health_analysis():

    print("===================")
    print("请输入健康情况")
    print("输入exit退出")
    print("===================")


    while True:

        user_input = input("\n健康信息:\n")


        if user_input.lower() == "exit":
            break


        prompt = build_health_prompt(user_input)


        print("\n===================")
        print("AI分析:")
        print("===================")


        answer = llm_chat(prompt)


        if answer:

            print(answer)

        else:

            print("模型未返回有效结果")




def main(pdf):


    print("===================")
    print("已加载PDF")
    print("===================")


    # PDF OCR
    pdf_to_md(pdf)


    print("===================")
    print("进入健康分析")
    print("===================")


    health_analysis()




if __name__ == "__main__":


    if len(sys.argv) != 2:

        print(
            "用法: python3 main.py xxx.pdf"
        )

        exit(1)


    main(sys.argv[1])