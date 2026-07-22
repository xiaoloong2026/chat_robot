
ythc123@ythc:~$ tree handle_demo/
handle_demo/
├── bin
│   └── llama-server
├── lib
│   ├── libcommon.a
│   ├── libggml-base.so
│   ├── libggml-cpu.so
│   ├── libggml.so
│   ├── libllama-common-base.a
│   ├── libllama.so
│   ├── libllama-ui.a
│   ├── libmtmd.so
│   ├── libriscv.so
│   ├── libserver-context.a
│   ├── libstdc++.so.6
│   ├── libtcim_dev_ctrl.so
│   └── libtcim_runtime_lite.so
├── logs
│   ├── glm-ocr_0.9B_mmproj_20260721_104508.log
│   ├── glm-ocr_0.9B_mmproj_20260721_184834.log
│   ├── qwen36_27b_4chips_port8082_20260721_104333.log
│   ├── qwen36_27b_4chips_port8082_20260721_104442.log
│   └── qwen36_27b_4chips_port8082_20260721_184820.log
├── main.py
├── models
│   ├── glm-ocr_0.9b_8192_1_1
│   │   ├── HiModel_xh2_glm-ocr_0.9b_256_8k_b1_1chip_2cores_v1.3.0_20260519.gguf
│   │   └── mmproj_xh2_glm-ocr_0.9b_2cores_v1.3.0_20260519.gguf
│   └── HiModel_xh2_qwen3.6_27b_256_256k_b1_4chips_2cores_v1.3.0_20260603.gguf
├── readme.md
├── row_pdf
│   ├── 重大疾病范围.pdf
│   ├── test_1_page.pdf
│   ├── test_3_pages.pdf
│   └── test_first2.pdf
├── scripts
│   ├── makedown_to_llm.py
│   ├── pdf_to_makedown.py
│   └── __pycache__
│       ├── makedown_to_llm.cpython-310.pyc
│       └── pdf_to_makedown.cpython-310.pyc
├── tools
│   └── split_pdf.py
└── workspace
    ├── makedown
    │   ├── document.md
    │   ├── page_001.md
    │   └── page_002.md
    └── png
        ├── page_001.png
        └── page_002.png

12 directories, 38 files



export LLAMA_ENABLE_LAZY_MODE=1
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./bin/llama-server 
  -m ./models/qwen3.5_0.8b_262144_1_1/HiModel_xh2_qwen3.5_0.8b_256_256k_b1_1chip_2cores_v1.3.0_20260429.gguf 
  --host 0.0.0.0 
  --port 8081 
  --presence_penalty 1.5

curl -s http://127.0.0.1:8081/v1/completions 
  -H "Content-Type: application/json" 
  -d '{
    "prompt": "用户：请写一篇不少于1500字的中文文章，主题是本地大模型部署。内容需要包括：1. 什么是本地大模型部署；2. 为什么需要GPU或NPU加速；3. 后摩LM5070这类推理卡在部署中的作用；4. llama-server的调用流程；5. OpenAI兼容接口的意义；6. 部署时常见问题和排查方法；7. 如何通过监控工具观察推理卡占用。请分段详细说明。\n助手：",
    "max_tokens": 2000,
    "temperature": 0.3,
    "stop": ["用户：", "\n用户"]
  }'

# 实时查看

watch -n 0.2 'hm_smi -a | grep -E "device[0-9]|IPU_Load|Core0_Util|Core1_Util|Average_Util|DDR_Memory_Free"'

# 生成带时间戳的日志文件

./bin/llama-server 
  -m ./models/qwen3.5_9b_262144_1_1/HiModel_xh2_qwen3.5_9b_256_256k_b1_1chip_2cores_v1.3.0_20260429.gguf 
  --host 127.0.0.1 
  --port 8081 
  --presence_penalty 1.5 
  2>&1 | tee ./logs/qwen35_9B_$(date +%Y%m%d_%H%M%S).log

./bin/llama-server 
  -m ./models/glm-ocr_0.9b_8192_1_1/HiModel_xh2_glm-ocr_0.9b_256_8k_b1_1chip_2cores_v1.3.0_20260519.gguf 
  --mmproj ./models/glm-ocr_0.9b_8192_1_1/mmproj_xh2_glm-ocr_0.9b_2cores_v1.3.0_20260519.gguf 
  --host 127.0.0.1 
  --port 8081 
  --presence_penalty 1.5 
  2>&1 | tee ./logs/glm-ocr_0.9B_mmproj_$(date +%Y%m%d_%H%M%S).log

# 三个窗口

./bin/llama-server   -m ./models/HiModel_xh2_qwen3.6_27b_256_256k_b1_4chips_2cores_v1.3.0_20260603.gguf   -dev 0,1,2,3   --host 127.0.0.1   --port 8082   --presence_penalty 1.5   2>&1 | tee ./logs/qwen36_27b_4chips_port8082_$(date +%Y%m%d_%H%M%S).log

./bin/llama-server   -m ./models/glm-ocr_0.9b_8192_1_1/HiModel_xh2_glm-ocr_0.9b_256_8k_b1_1chip_2cores_v1.3.0_20260519.gguf   --mmproj ./models/glm-ocr_0.9b_8192_1_1/mmproj_xh2_glm-ocr_0.9b_2cores_v1.3.0_20260519.gguf   --host 127.0.0.1   --port 8081   --presence_penalty 1.5   2>&1 | tee ./logs/glm-ocr_0.9B_mmproj_$(date +%Y%m%d_%H%M%S).log

./main.py row_pdf/test_1_pa
ge.pdf

test_prompt

健康状况：有慢性病 身高：172 cm 体重：80 kg 血压（收缩压）：158 mmHg 血压（舒张压）：98 mmHg 心率：92 次/分钟 需关注的既往疾病类型：高血压, 心血管病 有无严重传染病：无 心电图：（Base64编码文件）窦性心律，V1-V4导联ST段弓背向上抬高≥0.2mV，对应导联可见病理性Q波 血常规：（Base64编码文件）cTnI 25.3ng/mL（参考值<0.04），CK-MB 136U/L（参考值<25），MYO 580ng/mL

预期：
心电图ST段弓背向上抬高+心肌酶谱（cTnI、CK-MB）显著升高，符合急性心肌梗塞诊断标准四项中的典型心电图改变+心肌酶升高。明确命中重大疾病范围中「急性心肌梗塞」条款。建议立即就医，并确认冠脉造影结果。

2轮问答：
当时确实胸口疼得要命，去医院做了造影，医生说血管堵了95%，放了支架。对了，我记得重大疾病好像还有个什么冠状动脉搭桥术是不是也在这个范围里？

预期：

冠脉造影证实血管重度狭窄+支架植入术已实施。当前主要命中「急性心肌梗塞」条款。关于冠状动脉搭桥术：支架植入（PCI）不等于搭桥术（CABG），若后续因多支病变行开胸冠状动脉搭桥术则同时命中该条款。
