# v2.0-clientcmd

- Убран ложный server-side auto-XYZ как основной способ отдаления оружия: поля менялись, но клиент их не рендерил.
- Добавлена `point_clientcommand` entity.
- `!clientvm <value>` отправляет `viewmodel_fov <value>` на клиент для проверки доставки.
- `!clientfov <value>` отправляет `fov_cs_debug <value>` на клиент.
- `!fovexact <value>` совмещает server world/camera FOV и клиентский `fov_cs_debug`.
- Debug-команды больше не пишут `FOV_Value`, поэтому сохранённое VIP значение не должно перетирать тестовый FOV.
- VIP-меню по-прежнему сохраняет cookie и применяет выбранное значение.
