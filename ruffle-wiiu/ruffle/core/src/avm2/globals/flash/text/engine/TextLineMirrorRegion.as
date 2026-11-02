package flash.text.engine {
    import flash.events.EventDispatcher;
    import flash.geom.Rectangle;

    // The part of a line covered by an element with an eventMirror: mouse events on it go to that mirror.
    [API("662")]
    public final class TextLineMirrorRegion {
        internal var _bounds:Rectangle;
        internal var _element:ContentElement;
        internal var _mirror:EventDispatcher;
        internal var _textLine:TextLine;
        internal var _nextRegion:TextLineMirrorRegion;
        internal var _previousRegion:TextLineMirrorRegion;

        public function get bounds():Rectangle {
            return this._bounds;
        }

        public function get element():ContentElement {
            return this._element;
        }

        public function get mirror():EventDispatcher {
            return this._mirror;
        }

        public function get textLine():TextLine {
            return this._textLine;
        }

        public function get nextRegion():TextLineMirrorRegion {
            return this._nextRegion;
        }

        public function get previousRegion():TextLineMirrorRegion {
            return this._previousRegion;
        }
    }
}
