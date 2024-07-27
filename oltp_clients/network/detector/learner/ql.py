import random
import math
import numpy as np

from learn import Learner

# Q-Table q-learn agent.
class QT_Learner(Learner):
    def __init__(self, action_n, state_count, lr = 0.02, ga = 0.98):
        self.q = np.zeros((state_count, action_n), dtype=np.float64)
        self.action_space = action_n
        self.lr = lr
        self.ga = ga

    def get(self, s, a):
        return self.q[s][a]

    def put(self, s, a, q_):
        self.q[s][a] = q_

    def max_q(self, s):
        max_qv, max_action = -math.inf, 0
        for a in range(self.action_space):
            r = self.get(s, a)
            if r > max_qv:
                max_qv, max_action = r, a
        return max_action, max_qv

    def choose_action(self, s, eps):
        if random.random() < eps:
            return random.randint(0, self.action_space - 1)
        a, _ = self.max_q(s)
        return a

    def update_transition(self, s, a, r, s_t, done):
        print("trained")
        q = (1 - self.lr) * self.get(s, a) + self.lr * (r + self.ga * self.max_q(s_t)[1] * done)
        self.put(s, a, q)

    # Sacrifice the exploitation and have more exploration for training.
    def train_next_step(self, env, s, eps):
        a = self.choose_action(s, eps)
        s_, r, done, _ = env.step(a)
        self.update_transition(s, a, r, s_, 0. if done else 1.)
        return s_, r, done

# action 2: 0 for stop, 1 for next.
# space: 0 ~ 8 for , 8 for final.
def init():
    model = QT_Learner(2, 9)
    return model