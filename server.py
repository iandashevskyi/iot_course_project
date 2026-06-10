import json
import time
import paho.mqtt.client as mqtt
import numpy as np
from stable_baselines3 import DQN

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
TOPIC_SENSORS = "iot_proj/sensors"
TOPIC_ACTIONS = "iot_proj/actions"

try:
    print("Загрузка агента из agent.zip...")
    agent = DQN.load("agent.zip")
    print("Агент успешно загружен!")
except Exception as e:
    print(f"Ошибка загрузки агента: {e}")
    exit(1)

def validate_sensor_data(data):
    # Базовая валидация диапазонов (например, температура не больше 80)
    in_temp = data.get("in_temp", 25.0)
    in_hum = data.get("in_hum", 50.0)
    in_co2 = data.get("in_co2", 400.0)
    
    if in_temp > 80.0 or in_temp < -20.0:
        return None
    if in_hum > 100.0 or in_hum < 0.0:
        return None
    if in_co2 > 3000.0 or in_co2 < 300.0:
        return None
        
    return [in_temp, in_hum, in_co2]

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"Успешно подключились к MQTT-брокеру {MQTT_BROKER}")
        client.subscribe(TOPIC_SENSORS)
        print(f"Подписаны на топик: {TOPIC_SENSORS}")
    else:
        print(f"Ошибка подключения: {reason_code}")

def on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode("utf-8")
        data = json.loads(payload)
        print(f"[СЕРВЕР] Получены данные: {data}")
        
        #валидация данных
        valid_data = validate_sensor_data(data)
        if valid_data is None:
            print("[СЕРВЕР] Данные не прошли валидацию. Пропуск.")
            return
            
        #агент ожидает вектор (in_temp, in_hum, in_co2)
        obs = np.array(valid_data, dtype=np.float32)
        
        #получаем действие от агента
        action, _states = agent.predict(obs, deterministic=True)
        action_val = int(action) #действие - дискретное значение 0-63
        
        #фильтр действий
        #отправляем действие обратно по MQTT
        action_payload = json.dumps({"action": action_val})
        client.publish(TOPIC_ACTIONS, action_payload)
        print(f"[СЕРВЕР] Отправлено действие {action_val} в топик {TOPIC_ACTIONS}\n")
        
    except json.JSONDecodeError:
        print("[СЕРВЕР] Ошибка парсинга JSON")
    except Exception as e:
        print(f"[СЕРВЕР] Произошла ошибка: {e}")

#настройка клиента
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

print(f"Подключение к брокеру {MQTT_BROKER}...")
try:
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    #запуск цикла обработки
    client.loop_forever()
except KeyboardInterrupt:
    print("Остановка сервера...")
    client.disconnect()
except Exception as e:
    print(f"Не удалось подключиться к MQTT: {e}")
