# Ratostick

Gabriel Oliveira Santos (@gabrielsantos19)

## Introdução

Ratostick é um driver para joystick usb que traduz os dados enviados pelo joystick em eventos de mouse e teclado.

## Instalação

Compile o driver:
```
make
```

Instale o driver:
```
sudo insmod ratostick.ko
```

Atualmente este driver não tem maior prioridade que o driver usbhid. Isso significa que, mesmo após instalar este driver, ao plugar o joystick ele será atribuído ao driver usbhid. Assim, é necessário intervir para que o joystick seja atribuído ao driver ratostick. Uma das maneiras encontradas de realizar isso é remover o driver usbhid:
```
sudo rmmod usbhid
```

Após remover o driver usbhid, ao conectar o joystick ele será atribuído ao driver ratostick.

Outra maneira de atribuir o joystick ao driver ratostick é manualmente desvincular o joystick do usbhid e vincular ao ratostick [[1](https://lwn.net/Articles/143397/)]. Para desvincular:
```
echo -n "1-2:1.0" > /sys/bus/usb/drivers/usbhid/unbind
```

Para vincular o joystick ao ratostick:
```
echo -n "1-2:1.0" > /sys/bus/usb/drivers/ratostick/bind
```

### Comandos extras

Para remover o driver ratostick do sistema:
```
sudo rmmod ratostick
```
Para limpar os arquivos gerados pela compilação, incluindo o driver ratostick:
```
make clean
```

## Trabalhos futuros

1. Atualizar o driver para que ele funcione sem a necessidade de remover o módulo usbhid ou manualmente transferir o dispositivo para o driver.
