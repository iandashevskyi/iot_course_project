import json
import time
import random
import paho.mqtt.client as mqtt

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
TOPIC_SENSORS = "iot_proj/sensors"
TOPIC_ACTIONS = "iot_proj/actions"

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"[СИМУЛЯТОР] Подключен к {MQTT_BROKER}")
        client.subscribe(TOPIC_ACTIONS)
    else:
        print(f"[СИМУЛЯТОР] Ошибка подключения: {reason_code}")

def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8")
    print(f"\n[СИМУЛЯТОР ESP32] Получена команда (Action): {payload}")
    
    # Расшифровка дискретного действия 0-63
    try:
        data = json.loads(payload)
        action_val = data.get("action", 0)
        # Пример: 6 битов, где каждый бит означает включение определенного реле
        binary_action = format(action_val, '06b')
        print(f"   Реле 1 (Обогреватель): {'ВКЛ' if binary_action[5]=='1' else 'ВЫКЛ'}")
        print(f"   Реле 2 (Кондиционер):  {'ВКЛ' if binary_action[4]=='1' else 'ВЫКЛ'}")
        print(f"   Реле 3 (Увлажнитель):  {'ВКЛ' if binary_action[3]=='1' else 'ВЫКЛ'}")
        print(f"   Реле 4 (Окно 1):       {'ОТКР' if binary_action[2]=='1' else 'ЗАКР'}")
        print(f"   Реле 5 (Окно 2):       {'ОТКР' if binary_action[1]=='1' else 'ЗАКР'}")
        print(f"   Реле 6 (Вытяжка):      {'ВКЛ' if binary_action[0]=='1' else 'ВЫКЛ'}")
    except Exception as e:
        print(f"Ошибка парсинга команды: {e}")

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

print("Подключение к брокеру...")
try:
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_start() # Запускаем в фоновом потоке
    
    # Генерация данных
    while True:
        # Генерируем случайные данные, имитируя датчики (температура, влажность, CO2)
        sensor_data = {
            "in_temp": round(random.uniform(20.0, 30.0), 1),
            "in_hum": round(random.uniform(30.0, 60.0), 1),
            "in_co2": round(random.uniform(400.0, 1000.0), 1),
            "out_temp": round(random.uniform(-5.0, 15.0), 1),
            "out_hum": round(random.uniform(50.0, 90.0), 1),
            "out_co2": 400.0
        }
        
        payload = json.dumps(sensor_data)
        print(f"Отправка данных датчиков: {payload}")
        client.publish(TOPIC_SENSORS, payload)
        
        time.sleep(5) # Отправляем каждые 5 секунд

except KeyboardInterrupt:
    print("Остановка симулятора...")
    client.loop_stop()
    client.disconnect()
except Exception as e:
    print(f"Ошибка: {e}")
