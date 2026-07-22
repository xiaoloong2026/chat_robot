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





ythc123@ythc:~$ tree handle_1_page_demo/
handle_1_page_demo/
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
│   ├── qwen36_27b_4chips_port8082_20260721_104333.log
│   └── qwen36_27b_4chips_port8082_20260721_104442.log
├── main.py
├── models
│   ├── glm-ocr_0.9b_8192_1_1
│   │   ├── HiModel_xh2_glm-ocr_0.9b_256_8k_b1_1chip_2cores_v1.3.0_20260519.gguf
│   │   └── mmproj_xh2_glm-ocr_0.9b_2cores_v1.3.0_20260519.gguf
│   └── HiModel_xh2_qwen3.6_27b_256_256k_b1_4chips_2cores_v1.3.0_20260603.gguf
├── readme.md
├── row_pdf
│   ├── test_1_page.pdf
│   └── test_3_pages.pdf
├── scripts
│   ├── makedown_to_llm.py
│   ├── pdf_to_makedown.py
│   └── __pycache__
│       ├── makedown_to_llm.cpython-310.pyc
│       └── pdf_to_makedown.cpython-310.pyc
└── workspace
    ├── makedown
    │   ├── document.md
    │   ├── page_001.md
    │   ├── page_002.md
    │   ├── page_003.md
    │   └── page_004.md
    └── png
        ├── page_001.png
        ├── page_002.png
        ├── page_003.png
        └── page_004.png

11 directories, 37 files
