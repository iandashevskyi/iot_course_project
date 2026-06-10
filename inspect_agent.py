import sys
try:
    from stable_baselines3 import DQN
    import json
except ImportError as e:
    print(f"Error importing: {e}")
    sys.exit(1)

def inspect_agent():
    print("Loading agent.zip...")
    try:
        model = DQN.load("agent.zip")
        print("Agent loaded successfully.")
        
        print("\n--- Observation Space ---")
        print(model.observation_space)
        
        print("\n--- Action Space ---")
        print(model.action_space)
        
    except Exception as e:
        print(f"Failed to load: {e}")

if __name__ == "__main__":
    inspect_agent()
