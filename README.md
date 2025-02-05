wb-mqtt-logs
==========

Сервис, предоставляющий доступ к логам из journald через MQTT.

Протокол для получения данных логов: https://github.com/wirenboard/mqtt-rpc

Запросы MQTT RPC
================

List
-------------

Запрос возвращает список сеансов включения контроллера и список запущенных сервисов.

### Входные параметры 

Отсутствуют.

### Возвращаемое значение

JSON-объект со следующими полями:

* *boots* - массив из объектов с полями:
  * *hash* - [id сеанса](https://www.freedesktop.org/software/systemd/man/systemd.journal-fields.html#_BOOT_ID=);
  * *start* - временная метка начала сеанса (UNIX timestamp UTC);
  * *end* - - временная метка завершения сеанса (UNIX timestamp UTC), отсутствует для текущего сеанса;
* *services* - массив названий сервисов, установленных на контроллере

Load
-----------

Запрос возвращает записи из лога.

### Входные параметры

JSON-объект со следующими полями:

* *boot* - [id сеанса](https://www.freedesktop.org/software/systemd/man/systemd.journal-fields.html#_BOOT_ID=);
* *service* - название сервиса;
* *time* - временная метка первого сообщения в логе (UNIX timestamp UTC) в секундах;
* *levels* - массив с номерами уровней важности сообщений, "emerg" (0), "alert" (1), "crit" (2), "err" (3), "warning" (4), "notice" (5), "info" (6), "debug" (7). Если не указан, выбираются все сообщения;
* *pattern* - шаблон поиска сообщений, может содержать строку или регулярное выражение;
* *cursor* - объект с полями:
  * *id* - [уникальный идентификатор сообщения в journald](https://www.freedesktop.org/software/systemd/man/systemd.journal-fields.html#__CURSOR=);
  * *direction* - один из вариантов:
    * `forward` - запрос записей более поздних чем *id*;
    * `backward` - запрос записей более ранних чем *id*;
* *limit* - максимальное количество записей в ответе, но не более 100.

При наличии *time*, *cursor* игнорируется. В случае отсутствия *cursor* возвращается *limit* последних записей.

### Возвращаемое значение

JSON-массив объектов со следующими полями:
* *msg* - текст сообщения;
* *service* - название сервиса, передаётся, если не было указано в запросе; 
* *level* - [уровень сообщения](https://en.wikipedia.org/wiki/Syslog#Severity_level), не передаётся для уровня `SYS_INFO(6)`;
* *time* - временная метка (UNIX timestamp UTC) в миллисекундах;
* *cursor* - [уникальный идентификатор сообщения в journald](https://www.freedesktop.org/software/systemd/man/systemd.journal-fields.html#__CURSOR=). Может присутствовать в первом и последнем объекте массива.
