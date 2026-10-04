# Broker runs in Termux, started by name with app_process (shim/chameleon);
# nothing in the app itself calls it.
-keep class io.github.harryl0l.chameleon.Broker {
    public static void main(java.lang.String[]);
}
