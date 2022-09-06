from unittest import result
import requests

url = "http://192.168.1.103"

# result = requests.post(f"{url}/add_users", json={"users":[{"id":i,"name":"سیاوش ملایی"} for i in range(1, 101)]})

# result = requests.post(f"{url}/add_user", json={"id":3,"name":"رامین هاشم زاده خجسته"})

result = requests.post(f"{url}/set_config", json={"ntp":True,
                                                  "ntp_server":"pool.ntp.org",
                                                  "time_offset":16200,
                                                  "time_window":10,
                                                  "lat":38.067456,
                                                  "lon":46.308276,
                                                  "rad":10,
                                                  "volume":30,
                                                  "ssid":"Saymantech",
                                                  "pass":"miouch@meech1313",
                                                  "dhcp":True,
                                                  "ip":"192.168.1.103",
                                                  "mask":"255.255.255.128",
                                                  "gw":"192.168.1.1",
                                                  "dns":"192.168.1.50"})

# result = requests.delete(f"{url}/delete_all_users")

print(result.status_code, result.text)