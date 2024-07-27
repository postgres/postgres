import gym
import numpy as np

from ql import QT_Learner

def test_frozen_lake():
    env = gym.make("FrozenLake-v1")
    env.render()
    env.reset()
    agent = QT_Learner(env.action_space.n, env.observation_space.n)

    middle_win = 0
    for epoch in range(10000):
        if epoch % 1000 == 999:
            print("rate = ", middle_win / 1000)
            middle_win = 0
        s = env.reset()
        done = False
        epsilon = max(0.1, 0.3 - 0.3 * ((epoch + 1) / 10000))
        #Linear annealing from 20% to 10%
        while not done:
            s, r, done = agent.train_next_step(env, s, epsilon)
            if done and r == 1:
                middle_win += 1

    win = 0
    for i in range(1000):
        s = env.reset()
        done = False
        while not done:
            s, r, done = agent.next_step(env, s)
            if done and r == 1:
                win += 1

    print("win round = ", win)

if __name__ == '__main__':
    test_frozen_lake()