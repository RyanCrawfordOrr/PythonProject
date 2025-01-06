# dataset.py
import os
import cv2
import torch
import numpy as np
from torch.utils.data import Dataset
from torchvision.transforms import functional as F

class VehiclesPedestriansWeaponsDataset(Dataset):
    """
    Example dataset class for an object detection dataset.
    Classes:
      0: background (implicit)
      1: vehicle
      2: pedestrian
      3: weapon
    """

    def __init__(self, images_dir, annotations, transforms=None):
        """
        :param images_dir: directory containing images
        :param annotations: list of dicts or a path to annotation file
                            with bounding box details
        :param transforms: optional transforms (e.g. augmentations)
        """
        self.images_dir = images_dir
        self.annotations = annotations  # This might be a list of dicts
        self.transforms = transforms

    def __len__(self):
        return len(self.annotations)

    def __getitem__(self, idx):
        record = self.annotations[idx]
        filename = record["filename"]
        img_path = os.path.join(self.images_dir, filename)

        # Load image
        image = cv2.imread(img_path, cv2.IMREAD_COLOR)
        image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0

        # Prepare bounding boxes & labels
        boxes = []
        labels = []
        for box_info in record["bboxes"]:
            xmin = box_info["xmin"]
            ymin = box_info["ymin"]
            xmax = box_info["xmax"]
            ymax = box_info["ymax"]
            cls = box_info["label"]  # 1=vehicle, 2=pedestrian, 3=weapon

            boxes.append([xmin, ymin, xmax, ymax])
            labels.append(cls)

        boxes_tensor = torch.as_tensor(boxes, dtype=torch.float32)
        labels_tensor = torch.as_tensor(labels, dtype=torch.int64)

        target = {
            "boxes": boxes_tensor,
            "labels": labels_tensor
        }

        # Convert to a PyTorch tensor (CxHxW)
        if self.transforms:
            # Example if using torchvision transforms
            import torchvision.transforms as T
            image_tensor = F.to_tensor(image)  # shape: [3, H, W]
            # Then apply your augmentations
            image_tensor = self.transforms(image_tensor)
        else:
            # Minimal approach
            image_tensor = torch.tensor(image.transpose(2, 0, 1), dtype=torch.float32)

        return image_tensor, target
