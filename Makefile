LDFLAGS = `pkg-config --libs libevdev`
CFLAGS  = `pkg-config --cflags libevdev`

all: disable-touch-on-pen

.PHONY: clean
clean:
	rm -f disable-touch-on-pen

disable-touch-on-pen: main.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)
	
install-local: disable-touch-on-pen
	mkdir -p ~/.local/share/bin
	cp -f disable-touch-on-pen ~/.local/share/bin
	chmod +x ~/.local/share/bin/disable-touch-on-pen 
	mkdir -p ~/.local/share/applications
	cp -f disable-touch-on-pen.desktop ~/.local/share/applications
	sed -i "s@~@$(HOME)@g" ~/.local/share/applications/disable-touch-on-pen.desktop

uninstall-local:
	rm -f ~/.local/share/bin/disable-touch-on-pen 
	rm -f ~/.local/share/applications/disable-touch-on-pen.desktop
