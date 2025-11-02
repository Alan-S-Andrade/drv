class Clock(object):
    """
    A clock.
    """
    def __init__(self, hz):
        self.hz = hz

    @property
    def cycle(self):
        return 1.0 / self.hz

    @property
    def cycle_ps(self):
        return int(self.cycle * 1e12)

    def __int__(self):
        return self.hz

    def __mul__(self, other):
        return int(self.hz * other)

    def __str__(self):
        return f"{self.hz}"

if __name__ == '__main__':
    clk = Clock(2.0e9)
    print(f'{clk.cycle_ps}ps')
    print(f'{clk * 8:e}B/s')
