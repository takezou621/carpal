// config.local.example — これを config.local.js にコピーして値を設定する
// (config.local.js は .gitignore 済み。認証情報をコミットしないこと)
window.CARPAL_MQTT_URL = 'ws://localhost:9001';
window.CARPAL_MQTT_USER = 'webuser';
window.CARPAL_MQTT_PASS = '< MQTT_PASS の値を docker/backend/.env からコピー >';
window.CARPAL_TENANT_ID = 'default';
