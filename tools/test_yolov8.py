import time
import cv2
import numpy as np
import onnxruntime as ort

MODEL_PATH = "yolov8s.onnx"
CAM_INDEX = 0                  # /dev/video0
CAM_W, CAM_H = 640, 480
IMGSZ = 640
CONF, IOU = 0.25, 0.45

CLASSES = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
    "toothbrush",
]


# ---------- camera ----------
def open_camera():
    cap = cv2.VideoCapture(CAM_INDEX, cv2.CAP_V4L2)
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"YUYV"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAM_W)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAM_H)
    cap.set(cv2.CAP_PROP_CONVERT_RGB, 0)       # keep raw YUYV
    if not cap.isOpened():
        raise RuntimeError("Cannot open camera")
    return cap


def to_bgr(raw):
    """Convert a raw YUYV frame to BGR (passes through if already BGR)."""
    if raw.ndim == 3 and raw.shape[2] == 3:    # backend already converted
        return raw
    yuyv = np.asarray(raw, np.uint8).reshape(CAM_H, CAM_W, 2)
    return cv2.cvtColor(yuyv, cv2.COLOR_YUV2BGR_YUYV)


# ---------- pre / post processing ----------
def letterbox(img, size=IMGSZ):
    h, w = img.shape[:2]
    r = min(size / h, size / w)
    nh, nw = round(h * r), round(w * r)
    resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    top, left = (size - nh) // 2, (size - nw) // 2
    canvas = np.full((size, size, 3), 114, np.uint8)
    canvas[top:top + nh, left:left + nw] = resized
    return canvas, r, left, top


def preprocess(bgr):
    lb, r, left, top = letterbox(bgr)
    rgb = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    x = rgb.transpose(2, 0, 1)[None]           # NCHW (1,3,640,640)
    return np.ascontiguousarray(x), r, left, top


def postprocess(pred, shape, r, left, top):
    """pred: (1, 84, 8400) -> list of (x1, y1, x2, y2, score, cls)."""
    pred = pred[0].T                           # (8400, 84)
    boxes, cls_scores = pred[:, :4], pred[:, 4:]
    cls = cls_scores.argmax(1)
    scores = cls_scores.max(1)

    keep = scores > CONF
    boxes, scores, cls = boxes[keep], scores[keep], cls[keep]
    if len(boxes) == 0:
        return []

    xyxy = np.empty_like(boxes)
    xyxy[:, 0] = boxes[:, 0] - boxes[:, 2] / 2
    xyxy[:, 1] = boxes[:, 1] - boxes[:, 3] / 2
    xyxy[:, 2] = boxes[:, 0] + boxes[:, 2] / 2
    xyxy[:, 3] = boxes[:, 1] + boxes[:, 3] / 2

    xyxy[:, [0, 2]] = ((xyxy[:, [0, 2]] - left) / r).clip(0, shape[1])
    xyxy[:, [1, 3]] = ((xyxy[:, [1, 3]] - top) / r).clip(0, shape[0])

    xywh = np.column_stack([xyxy[:, :2], xyxy[:, 2:] - xyxy[:, :2]])
    idx = cv2.dnn.NMSBoxes(xywh.tolist(), scores.tolist(), CONF, IOU)
    idx = np.array(idx).flatten().astype(int)
    return [(*xyxy[i], scores[i], cls[i]) for i in idx]


def draw(img, dets):
    for x1, y1, x2, y2, s, c in dets:
        p1, p2 = (int(x1), int(y1)), (int(x2), int(y2))
        cv2.rectangle(img, p1, p2, (0, 255, 0), 2)
        cv2.putText(img, f"{CLASSES[int(c)]} {s:.2f}", (p1[0], max(p1[1] - 5, 12)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1, cv2.LINE_AA)


# ---------- main ----------
def main():
    sess = ort.InferenceSession(MODEL_PATH, providers=["CPUExecutionProvider"])
    inp_name = sess.get_inputs()[0].name
    cap = open_camera()

    fps, t_prev = 0.0, time.time()
    try:
        while True:
            ok, raw = cap.read()
            if not ok:
                print("Frame grab failed")
                break

            frame = to_bgr(raw)                          # YUYV -> BGR
            x, r, left, top = preprocess(frame)
            pred = sess.run(None, {inp_name: x})[0]
            dets = postprocess(pred, frame.shape, r, left, top)

            draw(frame, dets)
            now = time.time()
            fps = 0.9 * fps + 0.1 * (1.0 / (now - t_prev))
            t_prev = now
            cv2.putText(frame, f"FPS {fps:.1f}", (10, 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

            cv2.imshow("YOLOv8s ONNX", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
