import json
import time
import paho.mqtt.client as mqtt
import numpy as np
import os
import shutil
from stable_baselines3 import DQN

MQTT_BROKER = "172.16.22.167"
MQTT_PORT = 1883
TOPIC_SENSORS = "iot_proj/sensors"
TOPIC_ACTIONS = "iot_proj/actions"
TOPIC_TARGETS = "iot_proj/targets"

# Глобальные комфортные диапазоны по умолчанию
global_targets = {
    "temp_min": 21.0,
    "temp_max": 24.0,
    "hum_min": 40.0,
    "hum_max": 60.0,
    "co2_max": 800.0
}

try:
    if os.path.exists("dqn_climate_agent"):
        print("Упаковка агента из папки dqn_climate_agent...")
        shutil.make_archive("agent", "zip", "dqn_climate_agent")
        
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

# Фильтры действий удалены по просьбе пользователя. Агенту полностью доверяем.

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"Успешно подключились к MQTT-брокеру {MQTT_BROKER}")
        client.subscribe(TOPIC_SENSORS)
        client.subscribe(TOPIC_TARGETS)
        print(f"Подписаны на топики: {TOPIC_SENSORS}, {TOPIC_TARGETS}")
    else:
        print(f"Ошибка подключения: {reason_code}")

def on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode("utf-8")
        data = json.loads(payload)
        
        if msg.topic == TOPIC_TARGETS:
            print(f"[СЕРВЕР] Получены новые настройки комфорта: {data}")
            if "temperatureC" in data:
                global_targets["temp_min"] = data["temperatureC"].get("min", global_targets["temp_min"])
                global_targets["temp_max"] = data["temperatureC"].get("max", global_targets["temp_max"])
            if "humidityPct" in data:
                global_targets["hum_min"] = data["humidityPct"].get("min", global_targets["hum_min"])
                global_targets["hum_max"] = data["humidityPct"].get("max", global_targets["hum_max"])
            if "co2Ppm" in data:
                global_targets["co2_max"] = data["co2Ppm"].get("max", global_targets["co2_max"])
            return

        print(f"[СЕРВЕР] Получены данные датчиков: {data}")
        
        # Валидация данных
        valid_data = validate_sensor_data(data)
        if valid_data is None:
            print("[СЕРВЕР] Данные не прошли валидацию. Пропуск.")
            return
            
        # Агент ожидает вектор из 8 значений (in_temp, in_hum, in_co2, target_temp_min, target_temp_max, target_hum_min, target_hum_max, target_co2_max)
        obs_list = valid_data + [
            global_targets["temp_min"],
            global_targets["temp_max"],
            global_targets["hum_min"],
            global_targets["hum_max"],
            global_targets["co2_max"]
        ]
        obs = np.array(obs_list, dtype=np.float32)
        
        # Получаем действие от агента
        action, _states = agent.predict(obs, deterministic=True)
        action_val = int(action) # Действие - дискретное значение 0-63
        
        # Отправляем сырое действие обратно по MQTT (без фильтров)
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
