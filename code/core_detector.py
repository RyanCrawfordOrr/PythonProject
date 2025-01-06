# core_detector.py
import torch
import torchvision.transforms as T
from torchvision.models.detection import retinanet_resnet50_fpn, RetinaNet_ResNet50_FPN_Weights

labels_map = {
    1: "person", 2: "bicycle", 3: "car", 4: "motorcycle", 5: "airplane",
    6: "bus", 7: "train", 8: "truck", 9: "boat", 10: "traffic light",
    11: "fire hydrant", 12: "stop sign", 13: "parking meter", 14: "bench",
    15: "bird", 16: "cat", 17: "dog", 18: "horse", 19: "sheep",
    20: "cow", 21: "elephant", 22: "bear", 23: "zebra", 24: "giraffe",
    25: "backpack", 26: "umbrella", 27: "handbag", 28: "tie", 29: "suitcase",
    30: "frisbee", 31: "skis", 32: "snowboard", 33: "sports ball", 34: "kite",
    35: "baseball bat", 36: "baseball glove", 37: "skateboard", 38: "surfboard",
    39: "tennis racket", 40: "bottle", 41: "wine glass", 42: "cup", 43: "fork",
    44: "knife", 45: "spoon", 46: "bowl", 47: "banana", 48: "apple",
    49: "sandwich", 50: "orange", 51: "broccoli", 52: "carrot", 53: "hot dog",
    54: "pizza", 55: "donut", 56: "cake", 57: "chair", 58: "couch",
    59: "potted plant", 60: "bed", 61: "dining table", 62: "toilet", 63: "TV",
    64: "laptop", 65: "mouse", 66: "remote", 67: "keyboard", 68: "cell phone",
    69: "microwave", 70: "oven", 71: "toaster", 72: "sink", 73: "refrigerator",
    74: "book", 75: "clock", 76: "vase", 77: "scissors", 78: "teddy bear",
    79: "hair drier", 80: "toothbrush"
}

class Detector:
    def __init__(self, confidence_threshold=0.5,
                 weights=RetinaNet_ResNet50_FPN_Weights.DEFAULT,
                 device='cuda'):
        self.device = device
        self.model = retinanet_resnet50_fpn(weights=weights)
        self.model.eval()
        self.model.to(device)
        self.conf_threshold = confidence_threshold

    def detect(self, image):
        transform = T.Compose([
            T.ToTensor(),
        ])
        image_tensor = transform(image).to(self.device)
        with torch.no_grad():
            outputs = self.model([image_tensor])

        detections = []
        for box, score, label in zip(outputs[0]['boxes'], outputs[0]['scores'], outputs[0]['labels']):
            if score >= self.conf_threshold:
                label_name = labels_map.get(label.item(), "Unknown")
                detections.append({
                    "bbox": box.cpu().numpy(),
                    "score": score.item(),
                    "label": label.item(),
                    "label_name": label_name
                })
        return detections
