"""Image-derived motion estimate for diagnostics, not ground-truth engine velocity."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent/'dependencies'))
import cv2
import numpy as np

def observed_flow(previous,current):
    previous=np.asarray(previous,dtype=np.uint8)
    current=np.asarray(current,dtype=np.uint8)
    if previous.ndim!=2 or previous.shape!=current.shape:
        raise ValueError('Expected equally sized grayscale images')
    backward=cv2.calcOpticalFlowFarneback(current,previous,None,0.5,4,23,5,7,1.5,0)
    forward=cv2.calcOpticalFlowFarneback(previous,current,None,0.5,4,23,5,7,1.5,0)
    h,w=current.shape;yy,xx=np.mgrid[:h,:w].astype(np.float32)
    px=xx+backward[...,0];py=yy+backward[...,1]
    reverse=cv2.remap(forward,px,py,cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT,borderValue=0)
    closure=np.linalg.norm(backward+reverse,axis=-1)
    gx=cv2.Sobel(current,cv2.CV_32F,1,0,ksize=3)
    gy=cv2.Sobel(current,cv2.CV_32F,0,1,ksize=3)
    texture=cv2.boxFilter(gx*gx+gy*gy,-1,(9,9))
    confidence=(closure<0.75)&(texture>25)&(px>=1)&(px<w-2)&(py>=1)&(py<h-2)
    return backward,confidence
