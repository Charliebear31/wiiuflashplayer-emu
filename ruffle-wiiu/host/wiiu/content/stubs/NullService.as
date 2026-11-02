package {
    import flash.utils.Proxy;
    import flash.utils.flash_proxy;

    use namespace flash_proxy;

    // Answers for an online service that isn't there: any method does nothing, any property leads back here,
    // so a game's calls into a portal's API neither hang nor throw. Questions get "no": isGuest is yes.
    public dynamic class NullService extends Proxy {
        private var _name:String;

        public function NullService(name:String = "") {
            this._name = name;
        }

        override flash_proxy function callProperty(name:*, ...rest):* {
            var n:String = String(name);
            if (n == "isGuest") {
                return true;
            }
            if (n == "getUsername" || n == "getUserName" || n == "getName") {
                return "Guest";
            }
            if (n.indexOf("is") == 0 || n.indexOf("has") == 0 || n.indexOf("can") == 0) {
                return false;
            }
            if (n.indexOf("get") == 0) {
                return null;
            }
            return undefined;
        }

        override flash_proxy function getProperty(name:*):* {
            return this;
        }

        override flash_proxy function setProperty(name:*, value:*):void {
        }

        override flash_proxy function hasProperty(name:*):Boolean {
            return true;
        }

        public function toString():String {
            return "[NullService " + this._name + "]";
        }
    }
}
