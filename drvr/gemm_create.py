import numpy as np
np.random.seed(0)
A = np.random.randint(-127, 128, size=(256,128), dtype=np.int8)
B = np.random.randint(-127, 128, size=(128,512), dtype=np.int8)
A.tofile("A.bin")
B.tofile("B.bin")
