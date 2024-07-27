import numpy as np
import random
import remote

class QT_env():
    def __init__(self, state, actions, transit, level):
        self.n = len(actions)
        self.actions = actions
        self.r = np.zeros((len(transit), len(actions)), dtype=np.float)
        self.tr = transit
        self.state = state
        self.level = level # 2 or 3

    def reset(self): # restart from the beginning.
        self.state = 0

    def step(self, action):
        if action == 1:
            # the transit into more steps.
            rev = 2 ** max(self.state - 1, 0) * remote.getDelta(self.level)
        else:
            rev = remote.getValidation(self.level)
        self.r[self.state][action] = rev
        st = self.tr[self.state][action]
        return st, rev, st == len(self.tr)-1, None

    def sample(self):
        return random.randint(0, len(self.actions)-1)