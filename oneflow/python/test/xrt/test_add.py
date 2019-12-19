import unittest
import numpy as np

import oneflow as flow

config = flow.function_config()

def make_job(x_shape, y_shape, dtype=flow.float32):
    config.use_xla_jit(False)
    config.use_tensorrt(False)

    @flow.function(config)
    def add_job(x = flow.input_blob_def(x_shape, dtype=dtype),
                y = flow.input_blob_def(y_shape, dtype=dtype)):
        return x + y + x
    return add_job

def make_xla_job(x_shape, y_shape, dtype=flow.float32):
    config.use_xla_jit(True)
    config.use_tensorrt(False)

    @flow.function(config)
    def xla_add_job(x = flow.input_blob_def(x_shape, dtype=dtype),
                    y = flow.input_blob_def(y_shape, dtype=dtype)):
        return x + y + x
    return xla_add_job

def make_trt_job(x_shape, y_shape, dtype=flow.float32):
    config.use_xla_jit(False)
    config.use_tensorrt(True)

    @flow.function(config)
    def trt_add_job(x = flow.input_blob_def(x_shape, dtype=dtype),
                    y = flow.input_blob_def(y_shape, dtype=dtype)):
        return x + y + x
    return trt_add_job

class TestAdd(unittest.TestCase):
    def _test_body(self, x, y, dtype=np.float32):
        f1 = make_job(x.shape, y.shape, dtype=flow.float32)
        f2 = make_xla_job(x.shape, y.shape, dtype=flow.float32)
        f3 = make_trt_job(x.shape, y.shape, dtype=flow.float32)
        a = f1(x, y).get()
        b = f2(x, y).get()
        c = f3(x, y).get()
        print("without xla: ", a)
        print("with xla", b)
        print("with tensorrt", c)
        self.assertTrue(np.allclose(a, b , rtol=1e-03, atol=1e-05))
        self.assertTrue(np.allclose(a, c , rtol=1e-03, atol=1e-05))
        flow.clear_default_session()

    def _test_ones_body(self, x_shape, y_shape, dtype=np.float32):
        x = np.ones(x_shape, dtype=dtype)
        y = np.ones(y_shape, dtype=dtype)
        self._test_body(x, y, dtype=dtype)

    def _test_random_body(self, x_shape, y_shape, dtype=np.float32):
        x = np.random.random(x_shape).astype(dtype)
        y = np.random.random(y_shape).astype(dtype)
        self._test_body(x, y, dtype=dtype)

    def test_ones_input(self):
        self._test_ones_body((1, 10), (1, 10))
        self._test_ones_body((2, 10, 2), (2, 10, 2))
        self._test_ones_body((2, 5, 2, 2), (2, 5, 2, 2))

    def test_random_input(self):
        self._test_random_body((1, 10), (1, 10))
        self._test_random_body((2, 10, 2), (2, 10, 2))
        self._test_random_body((2, 5, 2, 2), (2, 5, 2, 2))

if __name__ == '__main__':
    unittest.main()
