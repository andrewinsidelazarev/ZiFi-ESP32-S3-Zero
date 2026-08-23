# HTTPS CA bundle

`x509_crt_bundle.bin` содержит корневые сертификаты Mozilla в компактном
формате ESP-IDF. Это общий набор центров сертификации, а не сертификаты
отдельных сайтов. Он встроен во flash и используется `WiFiClientSecure` для
проверки HTTPS-серверов; режим `setInsecure()` не применяется.

Исходный PEM: `https://curl.se/ca/cacert.pem`, получен 2026-08-23,
SHA-256 `F66DFF1BDF8F96060B8177976F8B7D9254BC89BC4DB933D769F7384D28480BC9`.

Генератор: ESP-IDF `v4.4.7`, файл
`components/mbedtls/esp_crt_bundle/gen_crt_bundle.py`, SHA-256
`1E8F0DC9FF99E20D888FD5A9CAB621E302ACF55111C52DBE19CA92BF2BCD3020`.

Результат: 121 сертификат, 55 587 байт, SHA-256
`49E7E1CA53F48330B1B507872F1447EB5F333632B6802282EC51AAAB5640787C`.
