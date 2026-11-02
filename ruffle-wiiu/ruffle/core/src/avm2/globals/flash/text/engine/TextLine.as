package flash.text.engine {
    import flash.display.DisplayObject;
    import flash.display.DisplayObjectContainer;
    import flash.errors.IllegalOperationError;
    import flash.events.MouseEvent;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.ui.ContextMenu;

    // FIXME: None of the DisplayObjectContainer methods actually work on
    // the TextLine class in Ruffle, despite the methods working fine in FP-
    // however, it's unlikely that SWFs will actually attempt to add children
    // to a TextLine.
    [Ruffle(Abstract)]
    [API("662")]
    public final class TextLine extends DisplayObjectContainer {
        public static const MAX_LINE_WIDTH:int = 1000000;

        public var userData;

        private var _mirrorRegions:Vector.<TextLineMirrorRegion> = null;
        private var _mirrorsHooked:Boolean = false;

        public native function get rawTextLength():int;

        public native function get textBlockBeginIndex():int;

        public native function get specifiedWidth():Number;

        public native function get textBlock():TextBlock;

        public native function get ascent():Number;

        [API("670")]
        public function get totalAscent():Number {
            return this.ascent;
        }

        public native function get descent():Number;

        [API("670")]
        public function get totalDescent():Number {
            return this.descent;
        }

        public function get unjustifiedTextWidth():Number {
            return this.textWidth;
        }

        public native function get textWidth():Number;
        public native function get textHeight():Number;

        public native function get validity():String;
        public native function set validity(value:String):void;

        public native function get hasGraphicElement():Boolean;

        private function blockText():String {
            return this.textBlock && this.textBlock.content ? this.textBlock.content.rawText : null;
        }

        // A surrogate pair is one atom; any other code unit is one too. Gives the unit each atom starts at, then the end.
        private function atomStarts():Array {
            var starts:Array = [];
            var text:String = this.blockText();
            var begin:int = this.textBlockBeginIndex;
            var end:int = begin + this.rawTextLength;
            for (var i:int = begin; i < end; i++) {
                starts.push(i);
                if (text != null && i + 1 < end) {
                    var unit:Number = text.charCodeAt(i);
                    if (unit >= 0xD800 && unit <= 0xDBFF) {
                        i++;
                    }
                }
            }
            starts.push(end);
            return starts;
        }

        private function checkedAtom(index:int):Array {
            var starts:Array = this.atomStarts();
            if (index < 0 || index >= starts.length - 1) {
                Error.throwError(RangeError, 2006);
            }
            return starts;
        }

        public function get atomCount():int {
            return this.atomStarts().length - 1;
        }

        public native function get previousLine():TextLine;
        public native function get nextLine():TextLine;

        public function getBaselinePosition(baseline:String):Number {
            switch (baseline) {
                case TextBaseline.ROMAN:
                    return 0.0;
                case TextBaseline.ASCENT:
                case TextBaseline.IDEOGRAPHIC_TOP:
                    return -this.ascent;
                case TextBaseline.DESCENT:
                case TextBaseline.IDEOGRAPHIC_BOTTOM:
                    return this.descent;
                case TextBaseline.IDEOGRAPHIC_CENTER:
                    return (this.descent - this.ascent) / 2;
            }
            Error.throwError(ArgumentError, 2004);
            return 0.0;
        }

        public native function get hasTabs():Boolean;

        private native function charLeft(index:int):Number;
        private native function charRight(index:int):Number;
        private native function graphicAt(index:int):DisplayObject;

        public function getAtomIndexAtPoint(stageX:Number, stageY:Number):int {
            var local:Point = this.globalToLocal(new Point(stageX, stageY));
            if (local.y < -this.ascent || local.y > this.descent) {
                return -1;
            }
            var starts:Array = this.atomStarts();
            var begin:int = this.textBlockBeginIndex;
            for (var i:int = 0; i < starts.length - 1; i++) {
                if (local.x >= this.charLeft(starts[i] - begin) && local.x < this.charRight(starts[i + 1] - 1 - begin)) {
                    return i;
                }
            }
            return -1;
        }

        public function getAtomIndexAtCharIndex(charIndex:int):int {
            var starts:Array = this.atomStarts();
            for (var i:int = 0; i < starts.length - 1; i++) {
                if (charIndex >= starts[i] && charIndex < starts[i + 1]) {
                    return i;
                }
            }
            return -1;
        }

        private static function isRightToLeft(code:Number):Boolean {
            return (code >= 0x0590 && code <= 0x08FF) || (code >= 0xFB1D && code <= 0xFDFF) || (code >= 0xFE70 && code <= 0xFEFF);
        }

        public function getAtomBidiLevel(index:int):int {
            var starts:Array = this.checkedAtom(index);
            var base:int = this.textBlock ? this.textBlock.bidiLevel : 0;
            var text:String = this.blockText();
            if (text != null && isRightToLeft(text.charCodeAt(starts[index]))) {
                return base | 1;
            }
            return base;
        }

        public function getAtomBounds(index:int):Rectangle {
            var starts:Array = this.checkedAtom(index);
            var begin:int = this.textBlockBeginIndex;
            var left:Number = this.charLeft(starts[index] - begin);
            var right:Number = this.charRight(starts[index + 1] - 1 - begin);
            if (right < left) {
                var swap:Number = left;
                left = right;
                right = swap;
            }
            return new Rectangle(left, -this.ascent, right - left, this.ascent + this.descent);
        }

        public function getAtomCenter(index:int):Number {
            var bounds:Rectangle = this.getAtomBounds(index);
            return bounds.x + bounds.width / 2;
        }

        public function getAtomGraphic(index:int):DisplayObject {
            var starts:Array = this.checkedAtom(index);
            return this.graphicAt(starts[index] - this.textBlockBeginIndex);
        }

        public function getAtomTextBlockBeginIndex(index:int):int {
            return this.checkedAtom(index)[index];
        }

        public function getAtomTextBlockEndIndex(index:int):int {
            return this.checkedAtom(index)[index + 1];
        }

        // The element holding the unit at `unit` of the block's text.
        private function elementAt(element:ContentElement, unit:int):ContentElement {
            if (element == null) {
                return null;
            }
            if (element is GroupElement) {
                var group:GroupElement = element as GroupElement;
                for (var i:int = 0; i < group.elementCount; i++) {
                    var found:ContentElement = this.elementAt(group.getElementAt(i), unit);
                    if (found != null) {
                        return found;
                    }
                }
                return null;
            }
            var start:int = element.textBlockBeginIndex;
            var length:int = element is GraphicElement ? 1 : (element.rawText == null ? 0 : element.rawText.length);
            return unit >= start && unit < start + length ? element : null;
        }

        public function getAtomTextRotation(index:int):String {
            var starts:Array = this.checkedAtom(index);
            var element:ContentElement = this.textBlock ? this.elementAt(this.textBlock.content, starts[index]) : null;
            return element ? element.textRotation : TextRotation.ROTATE_0;
        }

        public function getAtomWordBoundaryOnLeft(index:int):Boolean {
            var starts:Array = this.checkedAtom(index);
            if (index == 0) {
                return true;
            }
            var text:String = this.blockText();
            if (text == null) {
                return false;
            }
            return text.charAt(starts[index - 1]) == " " && text.charAt(starts[index]) != " ";
        }

        // Mirror regions: the parts of this line whose element has an eventMirror.

        public function get mirrorRegions():Vector.<TextLineMirrorRegion> {
            if (this._mirrorRegions == null) {
                this._mirrorRegions = this.buildMirrorRegions();
            }
            return this._mirrorRegions.length > 0 ? this._mirrorRegions : null;
        }

        private function buildMirrorRegions():Vector.<TextLineMirrorRegion> {
            var regions:Vector.<TextLineMirrorRegion> = new Vector.<TextLineMirrorRegion>();
            var block:TextBlock = this.textBlock;
            if (block == null || block.content == null) {
                return regions;
            }
            var begin:int = this.textBlockBeginIndex;
            this.collectMirrors(block.content, begin, begin + this.rawTextLength, regions);
            for (var i:int = 0; i < regions.length; i++) {
                regions[i]._previousRegion = i > 0 ? regions[i - 1] : null;
                regions[i]._nextRegion = i + 1 < regions.length ? regions[i + 1] : null;
            }
            return regions;
        }

        private function collectMirrors(element:ContentElement, begin:int, end:int, regions:Vector.<TextLineMirrorRegion>):void {
            if (element is GroupElement) {
                var group:GroupElement = element as GroupElement;
                for (var i:int = 0; i < group.elementCount; i++) {
                    this.collectMirrors(group.getElementAt(i), begin, end, regions);
                }
                return;
            }
            if (element.eventMirror == null) {
                return;
            }
            var start:int = element.textBlockBeginIndex;
            var length:int = element is GraphicElement ? 1 : (element.rawText == null ? 0 : element.rawText.length);
            var from:int = Math.max(start, begin);
            var to:int = Math.min(start + length, end);
            if (from >= to) {
                return;
            }
            var left:Number = Infinity, right:Number = -Infinity;
            for (var unit:int = from; unit < to; unit++) {
                left = Math.min(left, this.charLeft(unit - begin));
                right = Math.max(right, this.charRight(unit - begin));
            }
            var region:TextLineMirrorRegion = new TextLineMirrorRegion();
            region._textLine = this;
            region._element = element;
            region._mirror = element.eventMirror;
            region._bounds = new Rectangle(left, -this.ascent, right - left, this.ascent + this.descent);
            regions.push(region);
        }

        // Called when the line is (re)created: mouse events over a mirror region are passed on to its mirror.
        public function initMirrors():void {
            this._mirrorRegions = null;
            if (this._mirrorsHooked || this.mirrorRegions == null) {
                return;
            }
            this._mirrorsHooked = true;
            var types:Array = [MouseEvent.CLICK, MouseEvent.DOUBLE_CLICK, MouseEvent.MOUSE_DOWN, MouseEvent.MOUSE_UP,
                MouseEvent.MOUSE_MOVE, MouseEvent.MOUSE_OVER, MouseEvent.MOUSE_OUT, MouseEvent.ROLL_OVER,
                MouseEvent.ROLL_OUT, MouseEvent.MOUSE_WHEEL];
            for each (var type:String in types) {
                this.addEventListener(type, this.mirrorEvent);
            }
        }

        private function mirrorEvent(event:MouseEvent):void {
            var regions:Vector.<TextLineMirrorRegion> = this.mirrorRegions;
            if (regions == null) {
                return;
            }
            for each (var region:TextLineMirrorRegion in regions) {
                if (region.bounds.contains(event.localX, event.localY)) {
                    region.mirror.dispatchEvent(event.clone());
                    return;
                }
            }
        }

        // This function does nothing in Flash Player 32
        public function flushAtomData():void { }

        // Overrides

        override public function set contextMenu(cm:ContextMenu):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set focusRect(value:Object):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set tabChildren(value:Boolean):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set tabEnabled(value:Boolean):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        override public function set tabIndex(index:int):void {
            Error.throwError(IllegalOperationError, 2181);
        }

        // End of overrides
    }
}
