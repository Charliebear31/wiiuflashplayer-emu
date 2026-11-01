package {
    import flash.display.Sprite;

    // Stands in for Armor Games' online AGI.swf. Games wait for it to load before they start, and there's no
    // internet here: the game gets this instead, and its scoreboards just do nothing.
    // Built with tools\asc\asc.jar: -AS3 -strict -import playerglobal_import.abc -swf AGI,1,1,30 AGI.as
    public dynamic class AGI extends Sprite {
        public function init(devKey:String = null, gameKey:String = null):void {}
        public function initAGUI(... args):void {}
        public function showScoreboardSubmit(... args):void {}
        public function showScoreboardList(... args):void {}
        public function submitScore(... args):void {}
        public function retrieveScore(... args):void {}
        public function submitUserData(... args):void {}
        public function retrieveUserData(... args):void {}
        public function showLogin(... args):void {}
        public function closeAGUI(... args):void {}
        public function isLoggedIn():Boolean { return false; }
        public function getUserName():String { return ""; }
    }
}
