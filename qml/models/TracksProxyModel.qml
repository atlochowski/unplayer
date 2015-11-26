/*
 * Unplayer
 * Copyright (C) 2015 Alexey Rochev <equeim@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

import harbour.unplayer 0.1 as Unplayer

Unplayer.FilterProxyModel {
    function get(trackIndex) {
        return sourceModel.get(sourceIndex(trackIndex))
    }

    function getTracks() {
        var tracks = []
        var tracksCount = count()
        for (var i = 0; i < tracksCount; i++)
            tracks.push(sourceModel.get(sourceIndex(i)))
        return tracks
    }

    filterRoleName: "title"
}