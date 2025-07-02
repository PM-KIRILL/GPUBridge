#!/bin/bash

echo "Подключение к серверу GPUBridge по адресу: $GPU_SERVER"
echo "Используется бинарный файл GPUBridge по пути: $libgpu_path"

if [[ "$1" == "torch" ]]; then
    echo "Запуск примера с torch..."
    LD_PRELOAD="$libgpu_path" python3 -c "import torch; print('Доступность CUDA:', torch.cuda.is_available())"
elif [[ "$1" == "cublas" ]]; then
    echo "Запуск примера cublas..."
    LD_PRELOAD="$libsgpu_path" /matrixMulCUBLAS
elif [[ "$1" == "unified" ]]; then
    echo "Запуск примера с единой памятью..."
    LD_PRELOAD="$libgpu_path" /cublas_unified.o
else
    echo "Неизвестная опция: $1. Пожалуйста, укажите одну из: torch | cublas | unified ."
fi
