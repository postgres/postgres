class Learner():
    # Ask the next action with state s.
    def choose_action(self, s, eps):
        pass
    # Predict function for state s.
    def max_q(self, s):
        pass
    # The system transit from s to s_t with action a. The reward is r.
    def update_transition(self, s, a, r, s_t, done):
        pass
    # Sacrifice the exploitation and have more exploration for training.
    def train_next_step(self, env, s, eps):
        pass
    # Focus on the exploitation
    def next_step(self, env, s):
        a, _ = self.max_q(s)
        s_, r, done, _ = env.step(a)
        return s_, r, done
