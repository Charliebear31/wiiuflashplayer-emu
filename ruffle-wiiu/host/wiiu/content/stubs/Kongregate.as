package {
    import flash.display.Sprite;

    // Stands in for Kongregate's API_AS3_Local.swf: games get an object whose services do nothing.
    public dynamic class Kongregate extends Sprite {
        public var services:NullService = new NullService("services");
        public var user:NullService = new NullService("user");
        public var stats:NullService = new NullService("stats");
        public var scores:NullService = new NullService("scores");
        public var mtx:NullService = new NullService("mtx");
        public var images:NullService = new NullService("images");
        public var shared:NullService = new NullService("shared");
        public var chat:NullService = new NullService("chat");
        public var badges:NullService = new NullService("badges");
        public var kred:NullService = new NullService("kred");
        public var api:NullService = new NullService("api");

        public function Kongregate() {
        }
    }
}
