import QtQuick 2.0

Component {
    Component.onCompleted: {
        // Should cause an error since having either || or && on any side of the coalescing operator is banned by the specification
        var bad_rhs_or = 0 ?? 3 || 4;
    }
}
