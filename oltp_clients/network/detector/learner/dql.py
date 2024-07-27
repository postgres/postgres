import random
import math

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
import torch.nn.functional as F
import collections

# Hyper-parameters
buffer_limit  = 50000
batch_size    = 32
net_wide = 50

from learn import Learner

# from https://github.com/seungeunrho/minimalRL/blob/master/dqn.py
class ReplayBuffer():
    def __init__(self):
        self.buffer = collections.deque(maxlen=buffer_limit)

    def put(self, transition):
        self.buffer.append(transition)

    def sample(self, n):
        mini_batch = random.sample(self.buffer, n)
        s_lst, a_lst, r_lst, s_prime_lst, done_mask_lst = [], [], [], [], []

        for transition in mini_batch:
            s, a, r, s_prime, done_mask = transition
            s_lst.append(s)
            a_lst.append([a])
            r_lst.append([r])
            s_prime_lst.append(s_prime)
            done_mask_lst.append([done_mask])

        return torch.tensor(s_lst, dtype=torch.float), torch.tensor(a_lst), \
               torch.tensor(r_lst), torch.tensor(s_prime_lst, dtype=torch.float), \
               torch.tensor(done_mask_lst)

    def size(self):
        return len(self.buffer)

# DQN model.
class DQN(nn.Module):
    def __init__(self, inchan, num_actions):
        self.num_states = inchan
        self.num_actions = num_actions
        super(DQN, self).__init__()
        self.fc1 = nn.Linear(inchan, net_wide)
        self.fc2 = nn.Linear(net_wide, net_wide)
        self.fc3 = nn.Linear(net_wide, num_actions)

    def forward(self, x):
        x = F.relu(self.fc1(x))
        x = F.relu(self.fc2(x))
        x = self.fc3(x)
        return x

    def sample_action(self, state, eps):
        out = self.forward(state)
        coin = random.random()
        if coin < eps:
            action = random.randint(0, self.num_actions-1)
        else:
            action = out.argmax().item()
        return action, out[action]

class DQLearning(Learner):
    def __init__(self, action_space, in_dim, lr = 0.01, ga = 0.98):
        self.q = DQN(in_dim, action_space.n)
        self.q_target = DQN(in_dim, action_space.n)
        self.q_target.load_state_dict(self.q.state_dict())
        self.optimizer = optim.Adam(self.q.parameters(), lr = lr)
        self.action_space = action_space
        self.lr = lr
        self.ga = ga
        self.memory = ReplayBuffer()

    def train(self):
        for i in range(5):
            s, a, r, s_prime, done_mask = self.memory.sample(batch_size)

            q_out = self.q(s)
#            print(q_out)
            q_a = q_out.gather(1, a)
            max_q_prime = self.q_target(s_prime).max(1)[0].unsqueeze(1)
            target = r + self.ga * max_q_prime * done_mask
#            print(target)
            loss = F.smooth_l1_loss(q_a, target)
            self.optimizer.zero_grad()
            loss.backward()
#            print(loss.item())
            self.optimizer.step()

    def max_q(self, s):
        return self.q.sample_action(torch.tensor([s,], dtype=torch.float), 0)

    def choose_action(self, s, eps):
        return self.q.sample_action(s, eps)

    def update_transition(self, s, a, r, s_t, done):
        self.memory.put((s, a, r, s_t, done))
        if self.memory.size() > 1000:
            self.train()

    # Sacrifice the exploitation and have more exploration for training.
    def train_next_step(self, env, s, eps):
        a, _ = self.choose_action(torch.tensor([s,], dtype=torch.float), eps)
        s_, r, done, _ = env.step(a)
        self.update_transition(np.array([s,], dtype=np.float), a, r, np.array([s_,], dtype=np.float), 0. if done else 1.)
        return s_, r, done
