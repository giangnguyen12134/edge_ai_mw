from ultralytics import YOLO

model = YOLO("yolov8s.pt")

onnx_path = model.export(
    format="onnx",
    imgsz=640,       # input size
    opset=12,        # ONNX opset version
    simplify=True,   # run onnx-simplifier
    dynamic=False,   # set True for variable input shapes
    half=False,      # set True for FP16 (GPU only)
)
print("ONNX saved to:", onnx_path)

tflite_path = model.export(
    format="tflite",
    imgsz=640,
    int8=False,      # set True for INT8 quantization (needs calibration data)
    half=False,      # set True for an FP16 model
)
print("TFLite saved to:", tflite_path)
